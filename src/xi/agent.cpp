#include "xi/agent.hpp"

#include <algorithm>

#include "common/http_util.hpp"
#include "common/log.hpp"
#include "common/net.hpp"
#include "common/platform.hpp"
#include "common/sysinfo.hpp"
#include "xi/commands.hpp"
#include "xi/ui_assets.hpp"

namespace xi {

using xgi::json;
namespace fs = std::filesystem;

namespace {

std::string route_ip(const std::string& coordinator) {
    auto url = xgi::parse_base_url(coordinator);
    std::string ip = url ? xgi::route_ip_to(url->host, url->port) : "";
    return ip.empty() ? "127.0.0.1" : ip;
}

bool is_public_path(const std::string& p) { return p == "/" || p == "/ui" || p.rfind("/ui/", 0) == 0; }

std::string mime_type(const std::string& name) {
    if (xgi::ends_with_ci(name, ".html")) return "text/html; charset=utf-8";
    if (xgi::ends_with_ci(name, ".css")) return "text/css; charset=utf-8";
    if (xgi::ends_with_ci(name, ".js")) return "text/javascript; charset=utf-8";
    return "application/octet-stream";
}

}  // namespace

Agent::Agent(AgentBoot boot)
    : running_(boot.settings),
      configured_(boot.file.found ? boot.file_settings : boot.settings),
      token_(boot.token),
      token_source_(boot.token_source),
      file_(boot.file),
      cli_keys_(boot.cli_keys),
      exec_max_jobs_(boot.exec_max_jobs),
      started_wall_(xgi::wall_now()) {
    // Flags beat the file at startup, so what the page shows as "configured" includes them.
    for (const auto& k : cli_keys_) configured_ = apply_json(configured_, json{{k, to_json(running_)[k]}});
    resolved_ = resolve(running_);
    const fs::path log_dir = xgi::path_from_utf8(running_.log_dir);
    std::error_code ec;
    fs::create_directories(log_dir, ec);
    rpc_ = std::make_unique<ManagedProcess>("rpc", log_dir / "rpc.log");
    head_ = std::make_unique<ManagedProcess>("head", log_dir / "head.log");
    runtimes_ = detect_runtimes(log_dir / "exec.log");
    std::vector<std::string> names;
    for (const auto& r : runtimes_) names.push_back(r.id);
    xgi::log::info("runtimes: {}; console exec {}", names.empty() ? "none" : xgi::join(names, ", "),
                   running_.allow_exec ? "ENABLED" : "off");
    downloads_ = std::make_unique<Downloads>(
        [this] {
            auto snap = snapshot();
            return Downloads::Paths{snap.r.models_dir, xgi::path_from_utf8(snap.s.log_dir)};
        },
        [this] { reregister_ = true; });  // the coordinator learns about the new model file
    setup_routes();
}

Agent::~Agent() { stop(); }

Agent::Resolved Agent::resolve(const AgentSettings& s) {
    Resolved r;
    r.name = s.name.empty() ? xgi::sysinfo::hostname() : s.name;
    r.advertise_ip = !s.advertise_ip.empty() ? s.advertise_ip : s.coordinator.empty() ? "127.0.0.1" : route_ip(s.coordinator);
    fs::path llama = xgi::path_from_utf8(s.llama_dir);
    r.models_dir = xgi::path_from_utf8(s.models_dir);
    r.exec_dir = xgi::path_from_utf8(s.exec_dir);
    r.rpc_server_bin = s.rpc_server_bin.empty() ? llama / ("rpc-server" + std::string(xgi::kExeSuffix))
                                                : xgi::path_from_utf8(s.rpc_server_bin);
    r.llama_server_bin = s.llama_server_bin.empty() ? llama / ("llama-server" + std::string(xgi::kExeSuffix))
                                                    : xgi::path_from_utf8(s.llama_server_bin);
    return r;
}

Agent::Snapshot Agent::snapshot() const {
    std::lock_guard lk(cfg_mu_);
    return {running_, resolved_, token_};
}

std::vector<Runtime> Agent::runtimes() const {
    std::lock_guard lk(rt_mu_);
    return runtimes_;
}

void Agent::set_link(const std::function<void(Link&)>& fn) {
    std::lock_guard lk(link_mu_);
    fn(link_);
}

int Agent::default_threads() const { return xgi::sysinfo::cpu_physical(); }

json Agent::processes() { return json{{"rpc", rpc_->info().to_json()}, {"head", head_->info().to_json()}}; }

json Agent::facts() { return facts(snapshot()); }

json Agent::facts(const Snapshot& snap) {
    json rts = json::array();
    for (const auto& r : runtimes()) rts.push_back(r.to_json());
    std::error_code ec;
    auto space = fs::space(snap.r.models_dir, ec);
    bool space_ok = !ec;
    return json{
        {"node_id", snap.r.name},
        {"hostname", xgi::sysinfo::hostname()},
        {"ip", snap.r.advertise_ip},
        {"agent_port", snap.s.port},
        {"rpc_port", snap.s.rpc_port},
        {"head_port", snap.s.head_port},
        {"ram_total", xgi::sysinfo::ram_total()},
        {"ram_available", xgi::sysinfo::ram_available()},
        {"cpu_physical", xgi::sysinfo::cpu_physical()},
        {"cpu_logical", xgi::sysinfo::cpu_logical()},
        {"os", xgi::sysinfo::os_string()},
        {"arch", std::string(xgi::kArchName)},
        {"cpu_model", xgi::sysinfo::cpu_model()},
        {"disk_free", space_ok ? json(space.available) : json(nullptr)},
        {"models", scan_models(snap.r.models_dir)},
        {"processes", processes()},
        {"binaries", {{"rpc_server", fs::exists(snap.r.rpc_server_bin, ec)},
                      {"llama_server", fs::exists(snap.r.llama_server_bin, ec)}}},
        {"runtimes", rts},
        {"exec_enabled", snap.s.allow_exec},
        {"downloads", downloads_ ? downloads_->tool_json() : json{{"available", false}, {"tool", nullptr}}},
        {"agent_version", xgi::kVersion},
    };
}

// ------------------------------------------------------------------------------------------ routes

void Agent::setup_routes() {
    auto& s = server_;

    // X-Cluster-Token on everything except the web page's static files (ADR-004, ADR-010).
    s.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (is_public_path(req.path)) return httplib::Server::HandlerResponse::Unhandled;
        std::string token;
        {
            std::lock_guard lk(cfg_mu_);
            token = token_;
        }
        if (!xgi::check_cluster_token(req, res, token)) return httplib::Server::HandlerResponse::Handled;
        return httplib::Server::HandlerResponse::Unhandled;
    });

    setup_ui_routes();
    setup_download_routes();

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
        auto snap = snapshot();
        auto args = build_rpc_args(snap.s.rpc_port, r.threads.value_or(default_threads()), r.cache);
        std::lock_guard lk(start_mu_);
        try {
            xgi::reply_json(res, 200, rpc_->start(program_argv(snap.r.rpc_server_bin, args)).to_json());
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
        auto snap = snapshot();
        auto model = resolve_model_path(snap.r.models_dir, r.model_file);
        if (!model)
            return xgi::reply_error(res, 400, "model_file '" + r.model_file + "' is missing or outside the models dir",
                                    "invalid_request");
        auto args = build_head_args(*model, snap.s.head_port, r, r.threads.value_or(default_threads()));
        std::lock_guard lk(start_mu_);
        try {
            xgi::reply_json(res, 200, head_->start(program_argv(snap.r.llama_server_bin, args)).to_json());
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

    // Console code execution (ADR-009). Only when allowed, only with a detected runtime.
    s.Post("/exec", [this](const httplib::Request& req, httplib::Response& res) {
        auto snap = snapshot();
        if (!snap.s.allow_exec)
            return xgi::reply_error(res, 403, "code execution is disabled on this node (xi --allow-exec)", "forbidden");
        auto body = xgi::parse_body(req, res);
        if (!body) return;
        ExecRequest r;
        try {
            r = ExecRequest::from_json(*body, snap.s.exec_max_timeout_s);
        } catch (const std::exception& e) {
            return xgi::reply_error(res, 400, e.what(), "invalid_request");
        }
        auto rts = runtimes();
        auto rt = std::find_if(rts.begin(), rts.end(), [&](const Runtime& x) { return x.id == r.language; });
        if (rt == rts.end())
            return xgi::reply_error(res, 400, "language '" + r.language + "' is not available on this node",
                                    "invalid_request");
        if (exec_jobs_.fetch_add(1) >= exec_max_jobs_) {
            exec_jobs_.fetch_sub(1);
            res.set_header("Retry-After", "5");
            return xgi::reply_error(res, 429, "too many console jobs running on this node", "capacity");
        }
        xgi::log::info("exec: {} job from {} ({} bytes, timeout {} s)", r.language, req.remote_addr, r.code.size(),
                       r.timeout_s);
        try {
            json out = run_job(*rt, r, snap.r.exec_dir, xgi::path_from_utf8(snap.s.log_dir) / "exec.log");
            exec_jobs_.fetch_sub(1);
            xgi::log::info("exec: {} job finished: exit {} in {:.2f} s{}", r.language, out["exit_code"].dump(),
                           out["duration_s"].get<double>(), out["timed_out"].get<bool>() ? " (timed out)" : "");
            xgi::reply_json(res, 200, out);
        } catch (const std::exception& e) {
            exec_jobs_.fetch_sub(1);
            xgi::log::error("exec failed: {}", e.what());
            xgi::reply_error(res, 500, e.what(), "process");
        }
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

// The agent's own web page (02 §10, ADR-010).
void Agent::setup_ui_routes() {
    auto& s = server_;
    s.Get("/", [](const httplib::Request&, httplib::Response& res) { res.set_redirect("/ui/"); });
    s.Get("/ui", [](const httplib::Request&, httplib::Response& res) { res.set_redirect("/ui/"); });
    s.Get(R"(/ui/(.*))", [](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1];
        if (name.empty()) name = "index.html";
        const ui::Asset* a = ui::find_asset(name);
        if (!a) return xgi::reply_error(res, 404, "no such file", "not_found");
        res.set_header("Content-Security-Policy",
                       "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                       "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("X-Frame-Options", "DENY");
        res.set_header("Referrer-Policy", "no-referrer");
        res.set_header("Cache-Control", "no-cache");
        res.set_content(reinterpret_cast<const char*>(a->data), a->size, mime_type(name));
    });

    s.Get("/agent/state", [this](const httplib::Request&, httplib::Response& res) {
        auto snap = snapshot();
        Link link;
        {
            std::lock_guard lk(link_mu_);
            link = link_;
        }
        xgi::reply_json(res, 200,
                        json{{"version", xgi::kVersion},
                             {"started_at", started_wall_},
                             {"node", facts(snap)},
                             {"exec_jobs", exec_jobs_.load()},
                             {"link", {{"state", link.state},
                                       {"coordinator", link.coordinator},
                                       {"node_id", link.node_id},
                                       {"last_error", link.last_error},
                                       {"registered_at", link.registered_at},
                                       {"last_heartbeat_ok", link.last_heartbeat_ok}}}});
    });
    s.Get("/agent/settings", [this](auto& q, auto& r) { on_get_settings(q, r); });
    s.Put("/agent/settings", [this](auto& q, auto& r) { on_put_settings(q, r); });
    s.Post("/agent/register", [this](const httplib::Request&, httplib::Response& res) {
        reregister_ = true;
        xgi::reply_json(res, 202, json{{"ok", true}});
    });
    s.Post("/agent/runtimes/detect", [this](const httplib::Request&, httplib::Response& res) {
        std::unique_lock dl(detect_mu_, std::try_to_lock);
        if (!dl.owns_lock()) return xgi::reply_error(res, 409, "a detection is already running", "conflict");
        auto found = detect_runtimes(xgi::path_from_utf8(snapshot().s.log_dir) / "exec.log");
        json out = json::array();
        for (const auto& r : found) out.push_back(r.to_json());
        {
            std::lock_guard lk(rt_mu_);
            runtimes_ = std::move(found);
        }
        xgi::log::info("runtimes re-detected: {} found", out.size());
        reregister_ = true;  // tell the coordinator
        xgi::reply_json(res, 200, json{{"runtimes", out}});
    });
}

// Model downloads from Hugging Face / GitHub (02 §11, ADR-011).
void Agent::setup_download_routes() {
    auto& s = server_;
    auto guarded = [](httplib::Response& res, const std::function<void()>& fn) {
        try {
            fn();
        } catch (const DownloadError& e) {
            xgi::reply_error(res, e.status, e.what(), e.type);
        } catch (const std::invalid_argument& e) {
            xgi::reply_error(res, 400, e.what(), "invalid_request");
        }
    };
    s.Get("/downloads", [this](const httplib::Request&, httplib::Response& res) {
        xgi::reply_json(res, 200, downloads_->list());
    });
    s.Post("/downloads", [this, guarded](const httplib::Request& req, httplib::Response& res) {
        auto body = xgi::parse_body(req, res);
        if (!body) return;
        guarded(res, [&] {
            auto r = DownloadRequest::from_json(*body);
            xgi::log::info("download requested by {}: {} {} ({} file(s))", req.remote_addr,
                           xgi::hub::source_name(r.source), r.repo, r.files.size());
            xgi::reply_json(res, 201, downloads_->start(r));
        });
    });
    s.Post(R"(/downloads/([0-9a-f]+)/(pause|resume|cancel))", [this, guarded](const httplib::Request& req,
                                                                             httplib::Response& res) {
        std::string id = req.matches[1], action = req.matches[2];
        guarded(res, [&] {
            json out = action == "pause" ? downloads_->pause(id) : action == "resume" ? downloads_->resume(id)
                                                                                      : downloads_->cancel(id);
            xgi::reply_json(res, 200, out);
        });
    });
    s.Delete(R"(/downloads/([0-9a-f]+))", [this, guarded](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        guarded(res, [&] {
            downloads_->remove(id);
            xgi::reply_json(res, 200, json{{"ok", true}});
        });
    });
    s.Get("/hub/files", [this, guarded](const httplib::Request& req, httplib::Response& res) {
        guarded(res, [&] {
            auto src = xgi::hub::parse_source(req.get_param_value("source"));
            if (!src) throw std::invalid_argument("source must be huggingface or github");
            bool all = xgi::to_lower(req.get_param_value("all")) == "true" || req.get_param_value("all") == "1";
            xgi::reply_json(res, 200,
                            downloads_->hub_files(*src, req.get_param_value("repo"), req.get_param_value("ref"), all));
        });
    });
}

void Agent::on_get_settings(const httplib::Request& req, httplib::Response& res) {
    AgentSettings running, configured;
    std::string token_source;
    bool token_set;
    std::set<std::string> cli;
    SettingsLocation file;
    {
        std::lock_guard lk(cfg_mu_);
        running = running_;
        configured = configured_;
        token_source = token_source_;
        token_set = !token_.empty();
        cli = cli_keys_;
        file = file_;
    }
    std::vector<std::string> pending;
    for (const auto& k : changed_keys(running, configured))
        if (restart_keys().contains(k)) pending.push_back(k);
    AgentSettings blank = running;
    blank.name.clear();
    blank.advertise_ip.clear();
    blank.rpc_server_bin.clear();
    blank.llama_server_bin.clear();
    Resolved defaults = resolve(blank);
    json flags = json::object();
    for (const auto& k : cli) flags[k] = flag_for(k);
    xgi::reply_json(res, 200,
                    json{{"path", xgi::path_to_utf8(file.path)},
                         {"file_exists", file.found},
                         {"configured", to_json(configured)},
                         {"running", to_json(running)},
                         {"keys", settings_keys()},
                         {"restart_keys", restart_keys()},
                         {"restart_pending", pending},
                         {"cli_flags", flags},
                         {"token_set", token_set},
                         {"token_source", token_source},
                         {"local_request", is_loopback(req.remote_addr)},
                         {"defaults", {{"name", defaults.name},
                                       {"advertise_ip", defaults.advertise_ip},
                                       {"rpc_server_bin", xgi::path_to_utf8(defaults.rpc_server_bin)},
                                       {"llama_server_bin", xgi::path_to_utf8(defaults.llama_server_bin)}}}});
}

// Body: {"settings": {<keys to change>}, "token": "<new token>" (optional, not saved)}.
void Agent::on_put_settings(const httplib::Request& req, httplib::Response& res) {
    auto body = xgi::parse_body(req, res);
    if (!body) return;
    if (!body->is_object()) return xgi::reply_error(res, 400, "body must be a JSON object", "invalid_request");
    std::optional<std::string> new_token;
    if (body->contains("token") && !(*body)["token"].is_null()) {
        if (!(*body)["token"].is_string()) return xgi::reply_error(res, 400, "token must be a string", "invalid_request");
        if (!(*body)["token"].get<std::string>().empty()) new_token = (*body)["token"].get<std::string>();
    }

    std::lock_guard lk(cfg_mu_);  // one save at a time; readers take snapshots
    AgentSettings next;
    try {
        next = apply_json(configured_, body->value("settings", json::object()));
    } catch (const std::exception& e) {
        return xgi::reply_error(res, 400, e.what(), "invalid_request");
    }
    // Turning execution on is a local decision (ADR-010): the token alone must not be enough.
    if (next.allow_exec && !running_.allow_exec && !is_loopback(req.remote_addr))
        return xgi::reply_error(res, 403,
                                "code execution can only be turned on from this node itself (open http://127.0.0.1:" +
                                    std::to_string(running_.port) + "/ui/ on it), with --allow-exec, or in xi.yaml",
                                "forbidden");
    if (auto err = xgi::write_file_atomic(file_.path, to_yaml(next)); !err.empty())
        return xgi::reply_error(res, 500, err, "io");
    file_.found = true;

    AgentSettings before = running_;
    configured_ = next;
    running_ = merge_live(running_, next);
    resolved_ = resolve(running_);
    if (new_token) {
        token_ = *new_token;
        token_source_ = "ui";
    }
    std::vector<std::string> live = changed_keys(before, running_), restart, warnings;
    for (const auto& k : changed_keys(running_, configured_))
        if (restart_keys().contains(k)) restart.push_back(k);
    for (const auto& k : changed_keys(before, next))
        if (cli_keys_.contains(k))
            warnings.push_back(k + " is also set by " + flag_for(k) +
                               " on the command line, which wins at the next start. Remove the flag to keep this value.");
    if (new_token) {
        live.push_back("token");
        warnings.push_back("The new token is in effect now but is not saved: set QWEN_CLUSTER_TOKEN (or --token) "
                           "so it survives a restart.");
    }
    if (!live.empty()) reregister_ = true;
    xgi::log::info("settings saved to {} by {} (now: [{}]; after restart: [{}])", xgi::path_to_utf8(file_.path),
                   req.remote_addr, xgi::join(live, ", "), xgi::join(restart, ", "));
    xgi::reply_json(res, 200,
                    json{{"ok", true},
                         {"path", xgi::path_to_utf8(file_.path)},
                         {"live", live},
                         {"restart", restart},
                         {"warnings", warnings}});
}

// ------------------------------------------------------------------------------------------ control loop

bool Agent::do_register(const Snapshot& snap) {
    set_link([&](Link& l) {
        l.state = "registering";
        l.coordinator = snap.s.coordinator;
        l.node_id = snap.r.name;
    });
    auto r = xgi::http_call("POST", snap.s.coordinator, "/register", snap.token, facts(snap), 10);
    if (!r.ok) {
        xgi::log::warn("register with {} failed: {}", snap.s.coordinator, r.error);
        set_link([&](Link& l) {
            l.state = "error";
            l.last_error = "register: " + r.error;
        });
        return false;
    }
    double hb = r.body.is_object() ? r.body.value("heartbeat_interval_s", snap.s.heartbeat_s) : snap.s.heartbeat_s;
    if (hb != snap.s.heartbeat_s)
        xgi::log::warn("coordinator heartbeat_interval_s={} differs from heartbeat_s={}", hb, snap.s.heartbeat_s);
    xgi::log::info("registered as '{}' ({}) with {}", snap.r.name, snap.r.advertise_ip, snap.s.coordinator);
    set_link([&](Link& l) {
        l.state = "registered";
        l.last_error.clear();
        l.registered_at = l.last_heartbeat_ok = xgi::wall_now();
    });
    return true;
}

int Agent::heartbeat(const Snapshot& snap) {
    json body{{"node_id", snap.r.name}, {"ram_available", xgi::sysinfo::ram_available()}, {"processes", processes()}};
    auto r = xgi::http_call("POST", snap.s.coordinator, "/heartbeat", snap.token, body, 10);
    if (r.status == 0) xgi::log::warn("heartbeat failed: {}", r.error);
    else if (!r.ok && r.status != 404) xgi::log::warn("heartbeat rejected: {}", r.error);
    set_link([&](Link& l) {
        if (r.ok) {
            l.last_heartbeat_ok = xgi::wall_now();
            l.state = "registered";
        } else if (r.status != 404) {
            l.state = "error";
            l.last_error = "heartbeat: " + r.error;
        }
    });
    return r.status;
}

bool Agent::wait_or_wake(double seconds) {
    double until = xgi::mono_now() + seconds;
    while (xgi::mono_now() < until) {
        if (stop_flag_.wait_for(std::min(0.2, until - xgi::mono_now()))) return true;
        if (reregister_) return false;
    }
    return stop_flag_.stop_requested();
}

void Agent::control_loop() {
    // 02-node-agent.md §3: register (retry every 5 s) -> heartbeat loop; 404 -> re-register at once.
    // A settings change from the web page also re-registers. Children keep running throughout.
    bool registered = false;
    while (!stop_flag_.stop_requested()) {
        if (reregister_.exchange(false)) registered = false;
        auto snap = snapshot();
        if (snap.s.coordinator.empty()) {
            set_link([](Link& l) {
                l.state = "not_configured";
                l.coordinator.clear();
            });
            if (wait_or_wake(2.0)) break;
            continue;
        }
        if (!registered) {
            registered = do_register(snap);
            if (!registered) {
                if (wait_or_wake(5.0)) break;
                continue;
            }
        }
        if (wait_or_wake(snap.s.heartbeat_s)) break;
        if (reregister_) continue;
        int status = heartbeat(snapshot());
        if (status == 404) {
            xgi::log::info("coordinator does not know this node (404); re-registering");
            registered = false;
        } else if (status == 0) {
            registered = false;  // network error: re-register on the next attempt
        }
    }
}

bool Agent::run() {
    auto snap = snapshot();
    if (!server_.bind_to_port(snap.s.host, snap.s.port)) {
        xgi::log::error("cannot listen on {}:{}", snap.s.host, snap.s.port);
        return false;
    }
    xgi::log::info("XI {} node '{}' listening on {}:{} (advertise {}), coordinator {}", xgi::kVersion, snap.r.name,
                   snap.s.host, snap.s.port, snap.r.advertise_ip,
                   snap.s.coordinator.empty() ? "NOT SET (set it on the web page)" : snap.s.coordinator);
    xgi::log::info("models dir {} has {} GGUF file(s); auth {}; settings {} ({}); web page http://127.0.0.1:{}/ui/",
                   xgi::path_to_utf8(snap.r.models_dir), scan_models(snap.r.models_dir).size(),
                   snap.token.empty() ? "OFF" : "on", xgi::path_to_utf8(file_.path),
                   file_.found ? "loaded" : "not created yet", snap.s.port);
    control_thread_ = std::thread([this] { control_loop(); });
    server_.listen_after_bind();
    return true;
}

void Agent::request_shutdown() {
    // Stop the children before the HTTP server: on Linux they carry PR_SET_PDEATHSIG, which
    // fires when the server thread that started them exits, and would skip the graceful stop.
    stop_flag_.request_stop();
    downloads_->shutdown();  // pauses them: the .part files stay for a later resume
    {
        std::lock_guard lk(start_mu_);
        head_->stop(15);
        rpc_->stop(15);
    }
    server_.stop();
}

void Agent::stop() {
    if (stopped_.exchange(true)) return;
    xgi::log::info("shutting down: stopping child processes");
    stop_flag_.request_stop();
    server_.stop();
    if (control_thread_.joinable()) control_thread_.join();
    downloads_->shutdown();
    std::lock_guard lk(start_mu_);
    head_->stop(15);
    rpc_->stop(15);
}

}  // namespace xi
