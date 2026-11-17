#include "xgi/console.hpp"

#include <algorithm>
#include <format>
#include <future>

#include "common/http_util.hpp"
#include "common/hub.hpp"
#include "common/log.hpp"
#include "common/util.hpp"
#include "xgi/config_store.hpp"
#include "xgi/coordinator.hpp"
#include "xgi/ui_assets.hpp"

namespace xgi {

namespace {

constexpr size_t kMaxPromptBytes = 16 * 1024;
constexpr size_t kMaxHistoryTurns = 12;
constexpr size_t kMaxHistoryBytes = 64 * 1024;
constexpr size_t kMaxCodeBytes = 256 * 1024;  // = the agent's limit (05 §3)
constexpr double kExecOverheadS = 30;         // compile step, process start, network

std::string trim(std::string_view s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

// Static files must not be framed or sniffed, and may only talk back to this origin.
void security_headers(httplib::Response& res) {
    res.set_header("Content-Security-Policy",
                   "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                   "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Referrer-Policy", "no-referrer");
    res.set_header("Cache-Control", "no-cache");
}

std::string agent_url(const json& node) {
    return std::format("http://{}:{}", node.value("ip", ""), node.value("agent_port", 8200));
}

// The agent's error message, or the transport error.
std::string upstream_message(const HttpResult& r) {
    if (r.body.is_object() && r.body.contains("error") && r.body["error"].is_object())
        return r.body["error"].value("message", r.error);
    return r.error;
}

}  // namespace

// ------------------------------------------------------------------------------------------ helpers

std::string extract_code(std::string_view reply) {
    std::string text(reply);
    // Qwen3 "thinking" output: <think> … </think>. An unterminated block means the reply was cut off.
    for (size_t b; (b = text.find("<think>")) != std::string::npos;) {
        size_t e = text.find("</think>", b);
        text.erase(b, e == std::string::npos ? std::string::npos : e + 8 - b);
    }
    size_t fence = text.find("```");
    if (fence == std::string::npos) return trim(text);
    size_t body = text.find('\n', fence);
    if (body == std::string::npos) return trim(text.substr(fence + 3));
    ++body;
    // The closing fence is at the start of a line; a missing one means the reply hit max_tokens.
    size_t close = body;
    while (true) {
        close = text.find("```", close);
        if (close == std::string::npos || close == body || text[close - 1] == '\n') break;
        close += 3;
    }
    std::string code = close == std::string::npos ? text.substr(body) : text.substr(body, close - body);
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
    return code;
}

json build_generate_messages(const std::string& language, const json& node, const std::string& prompt,
                             const json& history) {
    std::string runtime = language, os = "the target node", arch;
    if (node.is_object()) {
        os = node.value("os", os);
        arch = node.value("arch", "");
        if (auto it = node.find("runtimes"); it != node.end() && it->is_array())
            for (const auto& r : *it)
                if (r.is_object() && r.value("language", "") == language) {
                    std::string label = r.value("label", language), version = r.value("version", "");
                    runtime = version.empty() ? label : std::format("{} ({})", label, version);
                }
    }
    std::string system = std::format(
        "You write programs that a cluster operator runs on a remote machine. Write one complete, self-contained "
        "{} program for the user's task. It runs unattended on {}{} using {}. Standard input is empty and there is "
        "a time limit, so never wait for input. Print results to standard output. Use only the standard library "
        "unless the user names a package. Reply with only the program in a single fenced code block and no "
        "explanation.",
        language, os, arch.empty() ? "" : " (" + arch + ")", runtime);
    json messages = json::array({{{"role", "system"}, {"content", system}}});
    if (history.is_array()) {
        size_t start = history.size() > kMaxHistoryTurns ? history.size() - kMaxHistoryTurns : 0;
        for (size_t i = start; i < history.size(); ++i) {
            const auto& m = history[i];
            if (!m.is_object() || !m["content"].is_string()) continue;
            std::string role = m.value("role", "");
            if (role != "user" && role != "assistant") continue;
            messages.push_back({{"role", role}, {"content", m["content"]}});
        }
    }
    messages.push_back({{"role", "user"}, {"content", prompt}});
    return messages;
}

std::string mime_type(std::string_view path) {
    auto ext = path.substr(std::min(path.size(), path.rfind('.') == std::string_view::npos ? path.size() : path.rfind('.')));
    std::string e = to_lower(std::string(ext));
    if (e == ".html") return "text/html; charset=utf-8";
    if (e == ".css") return "text/css; charset=utf-8";
    if (e == ".js") return "text/javascript; charset=utf-8";
    if (e == ".svg") return "image/svg+xml";
    if (e == ".json") return "application/json";
    return "application/octet-stream";
}

// ------------------------------------------------------------------------------------------ routes

Console::Console(const ClusterConfig& startup, ConfigStore& store, Coordinator& coordinator)
    : startup_(startup), store_(store), coord_(coordinator) {}

bool Console::is_public_path(const std::string& path) {
    return path == "/" || path == "/ui" || path.rfind("/ui/", 0) == 0;
}

void Console::register_routes(httplib::Server& s) {
    if (!startup_.console.enabled) return;
    s.Get("/", [](const httplib::Request&, httplib::Response& res) { res.set_redirect("/ui/"); });
    s.Get("/ui", [](const httplib::Request&, httplib::Response& res) { res.set_redirect("/ui/"); });
    s.Get(R"(/ui/(.*))", [](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1];
        if (name.empty()) name = "index.html";
        const ui::Asset* a = ui::find_asset(name);
        if (!a) return reply_error(res, 404, "no such file", "not_found");
        security_headers(res);
        res.set_content(reinterpret_cast<const char*>(a->data), a->size, mime_type(name));
    });
    s.Get("/console/info", [this](auto& q, auto& r) { on_info(q, r); });
    s.Post("/console/generate", [this](auto& q, auto& r) { on_generate(q, r); });
    s.Post("/console/exec", [this](auto& q, auto& r) { on_exec(q, r); });
    s.Get("/console/config", [this](auto& q, auto& r) { on_config_get(q, r); });
    s.Post("/console/config/check", [this](auto& q, auto& r) { on_config_post(q, r, false); });
    s.Put("/console/config", [this](auto& q, auto& r) { on_config_post(q, r, true); });
    s.Get("/console/hub/files", [this](auto& q, auto& r) { on_hub_files(q, r); });
    s.Get("/console/downloads", [this](auto& q, auto& r) { on_downloads_list(q, r); });
    s.Post("/console/downloads", [this](auto& q, auto& r) { on_downloads_start(q, r); });
    s.Post(R"(/console/downloads/([^/]+)/([0-9a-f]+)/(pause|resume|cancel))",
           [this](const httplib::Request& q, httplib::Response& r) { on_download_action(q, r, q.matches[3]); });
    s.Delete(R"(/console/downloads/([^/]+)/([0-9a-f]+))",
             [this](const httplib::Request& q, httplib::Response& r) { on_download_action(q, r, "remove"); });
}

void Console::on_info(const httplib::Request&, httplib::Response& res) {
    auto cfgp = store_.current();
    const ClusterConfig& cfg_ = *cfgp;
    json pools = json::array();
    for (const auto& p : cfg_.pools) pools.push_back(p.name);
    reply_json(res, 200,
               json{{"version", kVersion},
                    {"allow_exec", cfg_.console.allow_exec},
                    {"model", cfg_.console_model()},
                    {"pools", pools},
                    {"router_url", cfg_.console.router_url},
                    {"exec_timeout_s", cfg_.console.exec_timeout_s},
                    {"node_timeout_s", cfg_.coordinator.node_timeout_s}});
}

void Console::on_generate(const httplib::Request& req, httplib::Response& res) {
    auto body = parse_body(req, res);
    if (!body) return;
    if (!body->is_object() || !(*body)["language"].is_string() || !(*body)["prompt"].is_string())
        return reply_error(res, 400, "language and prompt are required", "invalid_request");
    std::string language = (*body)["language"], prompt = (*body)["prompt"];
    if (trim(prompt).empty()) return reply_error(res, 400, "prompt must not be empty", "invalid_request");
    if (prompt.size() > kMaxPromptBytes)
        return reply_error(res, 400, std::format("prompt is larger than {} bytes", kMaxPromptBytes), "invalid_request");
    json history = body->value("history", json::array());
    if (history.dump().size() > kMaxHistoryBytes)
        return reply_error(res, 400, "history is too large; start a new conversation", "invalid_request");

    auto cfgp = store_.current();
    const ClusterConfig& cfg_ = *cfgp;
    json node = nullptr;
    std::string node_id = body->is_object() && (*body)["node_id"].is_string() ? (*body)["node_id"].get<std::string>() : "";
    if (!node_id.empty()) {
        auto n = coord_.node_record(node_id);
        if (!n) return reply_error(res, 404, "unknown node " + node_id, "not_found");
        node = *n;
    }
    const std::string model = cfg_.console_model();
    if (model.empty()) return reply_error(res, 503, "no pool is configured for code generation", "unavailable");

    auto url = parse_base_url(cfg_.console.router_url);
    if (!url) return reply_error(res, 500, "console.router_url is not a valid http:// URL", "config");
    json request{{"model", model},
                 {"messages", build_generate_messages(language, node, prompt, history)},
                 {"max_tokens", cfg_.console.max_tokens},
                 {"temperature", 0.2},
                 {"stream", false}};
    httplib::Client cli(url->host, url->port);
    const auto t = static_cast<time_t>(cfg_.console.generate_timeout_s);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(t, 0);
    cli.set_write_timeout(30, 0);
    httplib::Headers headers;
    if (!cfg_.router.api_keys.empty()) headers.emplace("Authorization", "Bearer " + cfg_.router.api_keys.front());

    log::info("console: generate {} for {} via pool {} ({} bytes of prompt)", language, node_id.empty() ? "-" : node_id,
              model, prompt.size());
    const double started = mono_now();
    auto r = cli.Post("/v1/chat/completions", headers, request.dump(), "application/json");
    if (!r)
        return reply_error(res, 502, std::format("router {} unreachable: {}", url->origin(), httplib::to_string(r.error())),
                           "upstream");
    json out = json::parse(r->body, nullptr, false);
    if (r->status != 200) {
        std::string msg = out.is_object() && out.contains("error") && out["error"].is_object()
                              ? out["error"].value("message", r->body.substr(0, 200))
                              : r->body.substr(0, 200);
        int code = r->status == 401 ? 502 : r->status;  // a router key problem is ours, not the operator's
        return reply_error(res, code, "code generation failed: " + msg, "upstream");
    }
    std::string reply;
    try {
        reply = out.at("choices").at(0).at("message").at("content").get<std::string>();
    } catch (const std::exception&) {
        return reply_error(res, 502, "router reply has no choices[0].message.content", "upstream");
    }
    std::string finish = "";
    try {
        if (out["choices"][0]["finish_reason"].is_string()) finish = out["choices"][0]["finish_reason"];
    } catch (...) {
    }
    reply_json(res, 200,
               json{{"language", language},
                    {"code", extract_code(reply)},
                    {"reply", reply},
                    {"model", model},
                    {"group", r->get_header_value("X-Qwen-Group")},
                    {"finish_reason", finish},
                    {"duration_s", mono_now() - started}});
}

void Console::on_exec(const httplib::Request& req, httplib::Response& res) {
    auto cfgp = store_.current();
    const ClusterConfig& cfg_ = *cfgp;
    if (!cfg_.console.allow_exec)
        return reply_error(res, 403, "code execution is disabled (set console.allow_exec: true in cluster.yaml)",
                           "forbidden");
    auto body = parse_body(req, res);
    if (!body) return;
    if (!body->is_object() || !(*body)["node_id"].is_string() || !(*body)["language"].is_string() ||
        !(*body)["code"].is_string())
        return reply_error(res, 400, "node_id, language and code are required", "invalid_request");
    std::string node_id = (*body)["node_id"], language = (*body)["language"], code = (*body)["code"];
    if (code.empty() || code.size() > kMaxCodeBytes)
        return reply_error(res, 400, std::format("code must be 1 to {} bytes", kMaxCodeBytes), "invalid_request");
    double timeout = cfg_.console.exec_timeout_s;
    if (body->contains("timeout_s") && !(*body)["timeout_s"].is_null()) {
        if (!(*body)["timeout_s"].is_number() || (*body)["timeout_s"].get<double>() <= 0)
            return reply_error(res, 400, "timeout_s must be a number > 0", "invalid_request");
        timeout = (*body)["timeout_s"].get<double>();
    }

    auto node = coord_.node_record(node_id);
    if (!node) return reply_error(res, 404, "unknown node " + node_id, "not_found");
    if (node->value("status", "") != "alive") return reply_error(res, 409, "node " + node_id + " is dead", "conflict");
    if (!node->value("exec_enabled", false))
        return reply_error(res, 403, "node " + node_id + " does not allow code execution (xi --allow-exec)", "forbidden");
    std::string agent_url = std::format("http://{}:{}", node->value("ip", ""), node->value("agent_port", 8200));

    log::info("console: exec {} on {} ({} bytes, timeout {} s) requested by {}", language, node_id, code.size(), timeout,
              req.remote_addr);
    json agent_body{{"language", language}, {"code", code}, {"timeout_s", timeout}};
    auto r = http_call("POST", agent_url, "/exec", startup_.cluster_token, agent_body, timeout + kExecOverheadS);
    if (r.status == 0) return reply_error(res, 502, "agent on " + node_id + " unreachable: " + r.error, "upstream");
    if (!r.ok) {
        std::string msg = r.body.is_object() && r.body.contains("error") && r.body["error"].is_object()
                              ? r.body["error"].value("message", r.error)
                              : r.error;
        return reply_error(res, r.status, msg, "upstream");
    }
    json out = r.body.is_object() ? r.body : json::object();
    out["node_id"] = node_id;
    log::info("console: exec {} on {} finished: exit {}{}", language, node_id, out.value("exit_code", json()).dump(),
              out.value("timed_out", false) ? " (timed out)" : "");
    reply_json(res, 200, out);
}

void Console::on_config_get(const httplib::Request&, httplib::Response& res) { reply_json(res, 200, store_.describe()); }

// Body: {"yaml": "<cluster.yaml text>"} or {"config": {<cluster.yaml keys as JSON>}}.
void Console::on_config_post(const httplib::Request& req, httplib::Response& res, bool save) {
    auto body = parse_body(req, res);
    if (!body) return;
    ConfigCheck c;
    if (body->is_object() && (*body)["yaml"].is_string()) c = store_.check_yaml((*body)["yaml"].get<std::string>());
    else if (body->is_object() && body->contains("config")) c = store_.check_json((*body)["config"]);
    else return reply_error(res, 400, "send {\"yaml\": \"...\"} or {\"config\": {...}}", "invalid_request");
    if (!c.ok) return reply_json(res, 400, c.to_json());
    if (save) {
        log::info("console: config save requested by {}", req.remote_addr);
        c = store_.save(std::move(c));
        if (!c.ok) return reply_json(res, 500, c.to_json());
    }
    json out = c.to_json();
    out["saved"] = save;
    out["path"] = store_.describe()["path"];
    reply_json(res, 200, out);
}

// ------------------------------------------------------------------------------------------ downloads

// GET /console/hub/files?node_id=&source=&repo=&ref=&all= : the node lists the repo (it is the one
// that needs internet access), so this also checks that the node can reach the hub.
void Console::on_hub_files(const httplib::Request& req, httplib::Response& res) {
    std::string node_id = req.get_param_value("node_id");
    auto node = coord_.node_record(node_id);
    if (!node) return reply_error(res, 404, "unknown node " + node_id, "not_found");
    if (node->value("status", "") != "alive") return reply_error(res, 409, "node " + node_id + " is dead", "conflict");
    std::string path = "/hub/files?source=" + hub::encode_segment(req.get_param_value("source")) +
                       "&repo=" + hub::encode_segment(req.get_param_value("repo")) +
                       "&ref=" + hub::encode_segment(req.get_param_value("ref")) +
                       "&all=" + hub::encode_segment(req.get_param_value("all"));
    auto r = http_call("GET", agent_url(*node), path, startup_.cluster_token, nullptr, 75);
    if (r.status == 0) return reply_error(res, 502, "agent on " + node_id + " unreachable: " + r.error, "upstream");
    if (!r.ok) return reply_error(res, r.status, upstream_message(r), "upstream");
    json out = r.body;
    out["node_id"] = node_id;
    reply_json(res, 200, out);
}

// GET /console/downloads[?node_id=] : every alive node's download jobs, each with its node_id.
void Console::on_downloads_list(const httplib::Request& req, httplib::Response& res) {
    std::vector<json> nodes;
    if (req.has_param("node_id")) {
        auto n = coord_.node_record(req.get_param_value("node_id"));
        if (!n) return reply_error(res, 404, "unknown node " + req.get_param_value("node_id"), "not_found");
        nodes.push_back(*n);
    } else {
        for (const auto& n : coord_.nodes_json()) nodes.push_back(n);
    }
    std::vector<std::pair<std::string, std::future<HttpResult>>> calls;
    for (const auto& n : nodes) {
        if (n.value("status", "") != "alive") continue;
        if (n.contains("downloads") && n["downloads"].is_object() && !n["downloads"].value("available", true)) continue;
        std::string url = agent_url(n), token = startup_.cluster_token;
        calls.emplace_back(n.value("node_id", ""), std::async(std::launch::async, [url, token] {
                               return http_call("GET", url, "/downloads", token, nullptr, 4);
                           }));
    }
    json downloads = json::array(), errors = json::array();
    for (auto& [id, f] : calls) {
        HttpResult r = f.get();
        if (!r.ok) {
            // 404: an agent older than downloads. Not an error worth showing.
            if (r.status != 404) errors.push_back({{"node_id", id}, {"error", upstream_message(r)}});
            continue;
        }
        if (r.body.is_object() && r.body["downloads"].is_array())
            for (auto j : r.body["downloads"]) {
                j["node_id"] = id;
                downloads.push_back(std::move(j));
            }
    }
    std::sort(downloads.begin(), downloads.end(), [](const json& a, const json& b) {
        return a.value("created_at", 0.0) > b.value("created_at", 0.0);
    });
    reply_json(res, 200, json{{"downloads", downloads}, {"errors", errors}});
}

// POST /console/downloads {"node_ids": [...], "source", "repo", "ref", "files", "dest_dir"}
void Console::on_downloads_start(const httplib::Request& req, httplib::Response& res) {
    auto body = parse_body(req, res);
    if (!body) return;
    if (!body->is_object()) return reply_error(res, 400, "body must be a JSON object", "invalid_request");
    std::vector<std::string> ids;
    if (body->contains("node_ids") && (*body)["node_ids"].is_array())
        for (const auto& v : (*body)["node_ids"])
            if (v.is_string()) ids.push_back(v);
    if (body->contains("node_id") && (*body)["node_id"].is_string()) ids.push_back((*body)["node_id"]);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.empty()) return reply_error(res, 400, "node_ids must name at least one node", "invalid_request");
    json agent_body = *body;
    agent_body.erase("node_ids");
    agent_body.erase("node_id");

    json started = json::array(), errors = json::array();
    std::vector<std::pair<std::string, std::future<HttpResult>>> calls;
    for (const auto& id : ids) {
        auto n = coord_.node_record(id);
        if (!n) {
            errors.push_back({{"node_id", id}, {"code", 404}, {"error", "unknown node"}});
            continue;
        }
        if (n->value("status", "") != "alive") {
            errors.push_back({{"node_id", id}, {"code", 409}, {"error", "node is dead"}});
            continue;
        }
        std::string url = agent_url(*n), token = startup_.cluster_token;
        calls.emplace_back(id, std::async(std::launch::async, [url, token, agent_body] {
                               return http_call("POST", url, "/downloads", token, agent_body, 15);
                           }));
    }
    for (auto& [id, f] : calls) {
        HttpResult r = f.get();
        if (r.ok) {
            json j = r.body;
            j["node_id"] = id;
            started.push_back(j);
        } else {
            errors.push_back({{"node_id", id},
                              {"code", r.status == 0 ? 502 : r.status},
                              {"error", r.status == 0 ? "agent unreachable: " + r.error : upstream_message(r)}});
        }
    }
    const size_t nfiles = agent_body.contains("files") && agent_body["files"].is_array() ? agent_body["files"].size() : 0;
    log::info("console: download {} {} ({} file(s)) requested by {} on [{}]: {} started, {} failed",
              agent_body.value("source", std::string("?")), agent_body.value("repo", std::string("?")), nfiles,
              req.remote_addr, join(ids, ", "), started.size(), errors.size());
    int status = started.empty() ? errors.at(0).value("code", 502) : 200;
    json out{{"started", started}, {"errors", errors}};
    if (started.empty()) out["error"] = error_body(status, errors.at(0).value("error", std::string("failed")), "upstream")["error"];
    reply_json(res, status, out);
}

// POST /console/downloads/{node_id}/{id}/(pause|resume|cancel), DELETE /console/downloads/{node_id}/{id}
void Console::on_download_action(const httplib::Request& req, httplib::Response& res, const std::string& action) {
    std::string node_id = req.matches[1], id = req.matches[2];
    auto node = coord_.node_record(node_id);
    if (!node) return reply_error(res, 404, "unknown node " + node_id, "not_found");
    if (node->value("status", "") != "alive") return reply_error(res, 409, "node " + node_id + " is dead", "conflict");
    HttpResult r = action == "remove"
                       ? http_call("DELETE", agent_url(*node), "/downloads/" + id, startup_.cluster_token, nullptr, 10)
                       : http_call("POST", agent_url(*node), "/downloads/" + id + "/" + action, startup_.cluster_token,
                                   json::object(), 15);
    if (r.status == 0) return reply_error(res, 502, "agent on " + node_id + " unreachable: " + r.error, "upstream");
    if (!r.ok) return reply_error(res, r.status, upstream_message(r), "upstream");
    json out = r.body;
    if (out.is_object()) out["node_id"] = node_id;
    log::info("console: download {} on {}: {} (by {})", id, node_id, action, req.remote_addr);
    reply_json(res, 200, out);
}

}  // namespace xgi
