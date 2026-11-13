#include "xi/agent.hpp"

#include <algorithm>

#include "common/http_util.hpp"
#include "common/log.hpp"
#include "common/sysinfo.hpp"
#include "xi/commands.hpp"

namespace xi {

using xgi::json;

Agent::Agent(AgentOptions opts) : opts_(std::move(opts)) {
    if (opts_.name.empty()) opts_.name = xgi::sysinfo::hostname();
    if (opts_.rpc_server_bin.empty()) opts_.rpc_server_bin = opts_.llama_dir / "rpc-server.exe";
    if (opts_.llama_server_bin.empty()) opts_.llama_server_bin = opts_.llama_dir / "llama-server.exe";
    std::error_code ec;
    std::filesystem::create_directories(opts_.log_dir, ec);
    rpc_ = std::make_unique<ManagedProcess>("rpc", opts_.log_dir / "rpc.log");
    head_ = std::make_unique<ManagedProcess>("head", opts_.log_dir / "head.log");
    if (opts_.advertise_ip.empty()) opts_.advertise_ip = coordinator_host_ip();
    setup_routes();
}

Agent::~Agent() { stop(); }

std::string Agent::coordinator_host_ip() {
    auto url = xgi::parse_base_url(opts_.coordinator);
    std::string ip = url ? xgi::sysinfo::route_ip_to(url->host, url->port) : "";
    return ip.empty() ? "127.0.0.1" : ip;
}

int Agent::default_threads() const { return xgi::sysinfo::cpu_physical(); }

json Agent::processes() { return json{{"rpc", rpc_->info().to_json()}, {"head", head_->info().to_json()}}; }

json Agent::facts() {
    return json{
        {"node_id", opts_.name},
        {"hostname", xgi::sysinfo::hostname()},
        {"ip", opts_.advertise_ip},
        {"agent_port", opts_.port},
        {"rpc_port", opts_.rpc_port},
        {"head_port", opts_.head_port},
        {"ram_total", xgi::sysinfo::ram_total()},
        {"ram_available", xgi::sysinfo::ram_available()},
        {"cpu_physical", xgi::sysinfo::cpu_physical()},
        {"cpu_logical", xgi::sysinfo::cpu_logical()},
        {"os", xgi::sysinfo::os_string()},
        {"models", scan_models(opts_.models_dir)},
        {"processes", processes()},
        {"agent_version", xgi::kVersion},
    };
}

void Agent::setup_routes() {
    auto& s = server_;
    const std::string& token = opts_.token;

    s.set_pre_routing_handler([&token](const httplib::Request& req, httplib::Response& res) {
        if (!xgi::check_cluster_token(req, res, token)) return httplib::Server::HandlerResponse::Handled;
        return httplib::Server::HandlerResponse::Unhandled;
    });

    s.Get("/status", [this](const httplib::Request&, httplib::Response& res) { xgi::reply_json(res, 200, facts()); });

    s.Post("/rpc/start", [this](const httplib::Request& req, httplib::Response& res) {
        auto body = xgi::parse_body(req, res);
        if (!body) return;
        RpcStartRequest r;
        try {
            r = RpcStartRequest::from_json(*body);
        } catch (const std::exception& e) {
            return xgi::reply_error(res, 400, e.what(), "invalid_request");
        }
        auto args = build_rpc_args(opts_.rpc_port, r.threads.value_or(default_threads()), r.cache);
        std::lock_guard lk(start_mu_);
        try {
            xgi::reply_json(res, 200, rpc_->start(program_argv(opts_.rpc_server_bin, args)).to_json());
        } catch (const std::exception& e) {
            xgi::log::error("rpc start failed: {}", e.what());
            xgi::reply_error(res, 500, e.what(), "process");
        }
    });

    s.Post("/head/start", [this](const httplib::Request& req, httplib::Response& res) {
        auto body = xgi::parse_body(req, res);
        if (!body) return;
        HeadStartRequest r;
        try {
            r = HeadStartRequest::from_json(*body);
        } catch (const std::exception& e) {
            return xgi::reply_error(res, 400, e.what(), "invalid_request");
        }
        auto model = resolve_model_path(opts_.models_dir, r.model_file);
        if (!model)
            return xgi::reply_error(res, 400, "model_file '" + r.model_file + "' is missing or outside the models dir",
                                    "invalid_request");
        auto args = build_head_args(*model, opts_.head_port, r, r.threads.value_or(default_threads()));
        std::lock_guard lk(start_mu_);
        try {
            xgi::reply_json(res, 200, head_->start(program_argv(opts_.llama_server_bin, args)).to_json());
        } catch (const std::exception& e) {
            xgi::log::error("head start failed: {}", e.what());
            xgi::reply_error(res, 500, e.what(), "process");
        }
    });

    s.Post("/stop", [this](const httplib::Request& req, httplib::Response& res) {
        auto body = xgi::parse_body(req, res);
        if (!body) return;
        std::string role = body->is_object() ? body->value("role", "all") : "all";
        if (role != "rpc" && role != "head" && role != "all")
            return xgi::reply_error(res, 400, "role must be rpc, head or all", "invalid_request");
        std::lock_guard lk(start_mu_);
        // Head first: it holds connections to the workers.
        if (role == "head" || role == "all") head_->stop();
        if (role == "rpc" || role == "all") rpc_->stop();
        xgi::reply_json(res, 200, processes());
    });

    s.Get(R"(/logs/(rpc|head))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string role = req.matches[1];
        int lines = 100;
        if (req.has_param("lines")) {
            try {
                lines = std::stoi(req.get_param_value("lines"));
            } catch (...) {
                return xgi::reply_error(res, 400, "lines must be an integer", "invalid_request");
            }
        }
        lines = std::clamp(lines, 0, static_cast<int>(ManagedProcess::kTailLines));
        auto& p = role == "rpc" ? rpc_ : head_;
        xgi::reply_json(res, 200, json{{"role", role}, {"lines", p->tail(static_cast<size_t>(lines))}});
    });

    s.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "internal error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            msg = e.what();
        } catch (...) {
        }
        xgi::reply_error(res, 500, msg, "internal");
    });
}

bool Agent::do_register() {
    auto r = xgi::http_call("POST", opts_.coordinator, "/register", opts_.token, facts(), 10);
    if (!r.ok) {
        xgi::log::warn("register with {} failed: {}", opts_.coordinator, r.error);
        return false;
    }
    double hb = r.body.is_object() ? r.body.value("heartbeat_interval_s", opts_.heartbeat_s) : opts_.heartbeat_s;
    if (hb != opts_.heartbeat_s)
        xgi::log::warn("coordinator heartbeat_interval_s={} differs from --heartbeat-s={}", hb, opts_.heartbeat_s);
    xgi::log::info("registered as '{}' ({}) with {}", opts_.name, opts_.advertise_ip, opts_.coordinator);
    return true;
}

int Agent::heartbeat() {
    json body{{"node_id", opts_.name}, {"ram_available", xgi::sysinfo::ram_available()}, {"processes", processes()}};
    auto r = xgi::http_call("POST", opts_.coordinator, "/heartbeat", opts_.token, body, 10);
    if (r.status == 0) xgi::log::warn("heartbeat failed: {}", r.error);
    else if (!r.ok && r.status != 404) xgi::log::warn("heartbeat rejected: {}", r.error);
    return r.status;
}

void Agent::control_loop() {
    // 02-node-agent.md §3: register (retry every 5 s) -> heartbeat loop; 404 -> re-register at once.
    // Children keep running while the coordinator is unreachable.
    bool registered = false;
    while (!stop_flag_.is_set()) {
        if (!registered) {
            registered = do_register();
            if (!registered) {
                stop_flag_.wait_for(5.0);
                continue;
            }
        }
        if (stop_flag_.wait_for(opts_.heartbeat_s)) break;
        int status = heartbeat();
        if (status == 404) {
            xgi::log::info("coordinator does not know this node (404); re-registering");
            registered = false;
        } else if (status == 0) {
            registered = false;  // network error: re-register on the next attempt
        }
    }
}

bool Agent::run() {
    if (!server_.bind_to_port(opts_.host, opts_.port)) {
        xgi::log::error("cannot listen on {}:{}", opts_.host, opts_.port);
        return false;
    }
    xgi::log::info("XI {} node '{}' listening on {}:{} (advertise {}), coordinator {}", xgi::kVersion, opts_.name,
                   opts_.host, opts_.port, opts_.advertise_ip, opts_.coordinator);
    xgi::log::info("models dir {} has {} GGUF file(s); auth {}", xgi::narrow(opts_.models_dir.wstring()),
                   scan_models(opts_.models_dir).size(), opts_.token.empty() ? "OFF" : "on");
    control_thread_ = std::thread([this] { control_loop(); });
    server_.listen_after_bind();
    return true;
}

void Agent::request_shutdown() {
    stop_flag_.set();
    server_.stop();
}

void Agent::stop() {
    if (stopped_.exchange(true)) return;
    xgi::log::info("shutting down: stopping child processes");
    stop_flag_.set();
    server_.stop();
    if (control_thread_.joinable()) control_thread_.join();
    std::lock_guard lk(start_mu_);
    head_->stop(15);
    rpc_->stop(15);
}

}  // namespace xi
