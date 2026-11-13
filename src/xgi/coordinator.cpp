#include "xgi/coordinator.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "common/http_util.hpp"
#include "common/log.hpp"
#include "common/util.hpp"

namespace xgi {

namespace {

constexpr double kAgentStartTimeoutS = 30;  // start = stop old process (≤15 s grace) + spawn
constexpr double kAgentStopTimeoutS = 40;   // stop all = head + rpc, ≤15 s grace each
constexpr double kRpcPortTimeoutS = 60;     // 03 §5 step 3
constexpr int kHealthFailLimit = 3;         // 03 §6
constexpr double kMaxRepairBackoffS = 300;
// Heartbeats received this long after a start are trusted to describe the new process.
constexpr double kHeartbeatGraceS = 2;

json opt(const std::optional<std::string>& v) { return v ? json(*v) : json(nullptr); }
json opt(const std::optional<double>& v) { return v ? json(*v) : json(nullptr); }
json str_or_null(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

std::optional<long long> json_ll(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number_integer()) return j[key].get<long long>();
    return std::nullopt;
}

}  // namespace

std::string Coordinator::Endpoint::agent_url() const { return std::format("http://{}:{}", ip, agent_port); }

Coordinator::Coordinator(ClusterConfig cfg) : cfg_(std::move(cfg)) {}

Coordinator::~Coordinator() { stop(); }

// ------------------------------------------------------------------------------------------ routes

void Coordinator::register_routes(httplib::Server& s) {
    s.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard lk(mu_);
        size_t alive = std::count_if(nodes_.begin(), nodes_.end(), [](auto& kv) { return kv.second.alive; });
        reply_json(res, 200,
                   json{{"status", "ok"}, {"service", "xgi-coordinator"}, {"version", kVersion},
                        {"nodes_alive", alive}, {"deployed", deployed_}});
    });
    s.Post("/register", [this](auto& q, auto& r) { on_register(q, r); });
    s.Post("/heartbeat", [this](auto& q, auto& r) { on_heartbeat(q, r); });
    s.Get("/nodes", [this](auto&, auto& r) { reply_json(r, 200, nodes_json()); });
    s.Get("/groups", [this](auto&, auto& r) { reply_json(r, 200, groups_json()); });
    s.Post("/deploy", [this](auto& q, auto& r) { on_deploy(q, r); });
    s.Post("/undeploy", [this](auto& q, auto& r) { on_undeploy(q, r); });
    s.Post(R"(/groups/([^/]+)/restart)", [this](auto& q, auto& r) { on_restart(q, r); });
    s.Post(R"(/drain/([^/]+))", [this](auto& q, auto& r) { on_drain(q, r, true); });
    s.Post(R"(/undrain/([^/]+))", [this](auto& q, auto& r) { on_drain(q, r, false); });
}

void Coordinator::on_register(const httplib::Request& req, httplib::Response& res) {
    auto body = parse_body(req, res);
    if (!body) return;
    if (!body->is_object() || !(*body)["node_id"].is_string() || (*body)["node_id"].get<std::string>().empty() ||
        !(*body)["ip"].is_string())
        return reply_error(res, 400, "node_id and ip are required", "invalid_request");

    Node n;
    n.facts = *body;
    n.node_id = (*body)["node_id"];
    n.hostname = body->value("hostname", n.node_id);
    n.ip = (*body)["ip"];
    n.agent_port = body->value("agent_port", 8200);
    n.rpc_port = body->value("rpc_port", 50052);
    n.head_port = body->value("head_port", 8080);
    n.ram_available = body->value("ram_available", std::uint64_t{0});
    if (body->contains("models") && (*body)["models"].is_array())
        for (const auto& m : (*body)["models"])
            if (m.is_string()) n.models.insert(normalize_model_path(m.get<std::string>()));
    if (auto p = body->find("processes"); p != body->end() && p->is_object())
        for (auto& [role, info] : p->items())
            n.processes[role] = {info.value("state", "stopped"), json_ll(info, "pid"), json_ll(info, "exit_code")};
    n.last_seen = wall_now();
    n.last_seen_mono = mono_now();

    std::lock_guard lk(mu_);
    if (auto it = nodes_.find(n.node_id); it != nodes_.end()) {
        const Node& old = it->second;
        n.draining = old.draining;
        // Keep membership only if the group still lists this node (an agent restart while serving);
        // the monitor then sees the missing processes and repairs the group.
        if (Group* g = old.group_id.empty() ? nullptr : find_group_locked(old.group_id)) {
            bool member = g->head == n.node_id ||
                          std::find(g->workers.begin(), g->workers.end(), n.node_id) != g->workers.end();
            if (member) {
                n.group_id = old.group_id;
                n.role = old.role;
            }
        }
        log::info("node {} re-registered ({}:{}, {} model file(s))", n.node_id, n.ip, n.agent_port, n.models.size());
    } else {
        log::info("node {} registered ({}:{}, {} model file(s))", n.node_id, n.ip, n.agent_port, n.models.size());
    }
    nodes_[n.node_id] = std::move(n);
    reply_json(res, 200, json{{"ok", true}, {"heartbeat_interval_s", cfg_.coordinator.heartbeat_interval_s}});
}

void Coordinator::on_heartbeat(const httplib::Request& req, httplib::Response& res) {
    auto body = parse_body(req, res);
    if (!body) return;
    std::string id = body->is_object() ? body->value("node_id", "") : "";
    std::lock_guard lk(mu_);
    auto it = nodes_.find(id);
    // Unknown, or marked dead: 404 makes the agent re-register and rejoin as a free node.
    if (it == nodes_.end() || !it->second.alive) return reply_error(res, 404, "unknown node_id; register first", "not_found");
    Node& n = it->second;
    n.last_seen = wall_now();
    n.last_seen_mono = mono_now();
    if (body->contains("ram_available") && (*body)["ram_available"].is_number_unsigned()) {
        n.ram_available = (*body)["ram_available"].get<std::uint64_t>();
        n.facts["ram_available"] = n.ram_available;
    }
    if (auto p = body->find("processes"); p != body->end() && p->is_object()) {
        n.processes.clear();
        for (auto& [role, info] : p->items())
            n.processes[role] = {info.value("state", "stopped"), json_ll(info, "pid"), json_ll(info, "exit_code")};
        n.facts["processes"] = *p;
    }
    reply_json(res, 200, json{{"ok", true}});
}

void Coordinator::on_deploy(const httplib::Request& req, httplib::Response& res) {
    bool force = req.has_param("force") && to_lower(req.get_param_value("force")) == "true";
    std::lock_guard dlk(deploy_mu_);
    bool was_deployed;
    {
        std::lock_guard lk(mu_);
        was_deployed = deployed_;
    }
    if (was_deployed && !force)
        return reply_error(res, 409, "already deployed; use POST /deploy?force=true to redeploy", "conflict");
    if (was_deployed) undeploy_all();

    std::lock_guard lk(mu_);
    groups_.clear();
    int alive = 0;
    for (auto& [_, n] : nodes_) {
        if (n.alive) ++alive;
        n.group_id.clear();
        n.role.clear();
    }
    int requested = cfg_.total_nodes_requested() + cfg_.spares;
    if (requested > alive)
        log::warn("deploy: pools + spares need {} nodes but only {} are alive; extra groups stay unplaced", requested,
                  alive);
    for (const auto& pool : cfg_.pools) {
        for (int i = 0; i < pool.groups; ++i) {
            Group g;
            g.group_id = std::format("{}-{}", pool.name, i);
            g.pool = pool.name;
            g.slots = cfg_.slots_for(pool);
            groups_.push_back(std::move(g));
        }
    }
    deployed_ = true;
    for (auto& g : groups_) {
        if (try_place_locked(g, cfg_.spares)) launch_locked(g, TaskKind::start);
    }
    json out = json::array();
    for (const auto& g : groups_) out.push_back(group_json_locked(g));
    log::info("deploy: {} group(s) planned", groups_.size());
    reply_json(res, 200, json{{"groups", out}});
}

void Coordinator::on_undeploy(const httplib::Request&, httplib::Response& res) {
    std::lock_guard dlk(deploy_mu_);
    int n = undeploy_all();
    reply_json(res, 200, json{{"stopped", n}});
}

void Coordinator::on_restart(const httplib::Request& req, httplib::Response& res) {
    std::string id = req.matches[1];
    std::lock_guard lk(mu_);
    Group* g = find_group_locked(id);
    if (!g) return reply_error(res, 404, "unknown group " + id, "not_found");
    bool all_alive = !g->head.empty();
    for (const auto& ep : member_endpoints_locked(*g, false)) {
        auto it = nodes_.find(ep.node_id);
        if (it == nodes_.end() || !it->second.alive || it->second.draining) all_alive = false;
    }
    log::info("group {}: restart requested ({})", id, all_alive ? "same members" : "re-placing");
    g->repair_attempts = 0;
    g->error.reset();
    g->status = "starting";
    launch_locked(*g, all_alive ? TaskKind::restart : TaskKind::repair);
    reply_json(res, 202, json{{"group_id", id}, {"status", "starting"}});
}

void Coordinator::on_drain(const httplib::Request& req, httplib::Response& res, bool drain) {
    std::string id = req.matches[1];
    std::lock_guard lk(mu_);
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return reply_error(res, 404, "unknown node " + id, "not_found");
    Node& n = it->second;
    n.draining = drain;
    log::info("node {} {}", id, drain ? "draining" : "undrained");
    if (drain && !n.group_id.empty()) {
        if (Group* g = find_group_locked(n.group_id); g && g->status != "stopped") {
            g->status = "degraded";
            g->error = "node " + id + " drained";
            g->repair_attempts = 0;
            launch_locked(*g, TaskKind::repair);
        }
    }
    reply_json(res, 200, node_json_locked(n));
}

// ------------------------------------------------------------------------------------------ JSON

json Coordinator::nodes_json() {
    std::lock_guard lk(mu_);
    json out = json::array();
    for (const auto& [_, n] : nodes_) out.push_back(node_json_locked(n));
    return out;
}

json Coordinator::groups_json() {
    std::lock_guard lk(mu_);
    json out = json::array();
    for (const auto& g : groups_) out.push_back(group_json_locked(g));
    return out;
}

json Coordinator::node_json_locked(const Node& n) const {
    json j = n.facts;
    j["status"] = n.alive ? "alive" : "dead";
    j["draining"] = n.draining;
    j["group_id"] = str_or_null(n.group_id);
    j["role"] = str_or_null(n.role);
    j["last_seen"] = n.last_seen;
    return j;
}

json Coordinator::group_json_locked(const Group& g) const {
    return json{{"group_id", g.group_id},
                {"pool", g.pool},
                {"status", g.status},
                {"head", str_or_null(g.head)},
                {"workers", g.workers},
                {"head_url", str_or_null(g.head_url)},
                {"slots", g.slots},
                {"error", opt(g.error)},
                {"started_at", opt(g.started_at)},
                {"healthy_at", opt(g.healthy_at)}};
}

// ------------------------------------------------------------------------------------------ state helpers

Coordinator::Group* Coordinator::find_group_locked(const std::string& id) {
    for (auto& g : groups_)
        if (g.group_id == id) return &g;
    return nullptr;
}

bool Coordinator::is_current_locked(const std::string& gid, std::uint64_t gen) {
    Group* g = find_group_locked(gid);
    return g && g->generation == gen;
}

std::vector<FreeNode> Coordinator::free_nodes_locked() const {
    std::vector<FreeNode> out;
    for (const auto& [_, n] : nodes_)
        if (n.alive && !n.draining && n.group_id.empty())
            out.push_back(FreeNode{n.node_id, n.hostname, n.ram_available, n.models});
    return out;
}

std::set<std::string> Coordinator::pool_model_files() const {
    std::set<std::string> s;
    for (const auto& p : cfg_.pools) s.insert(p.model_file);
    return s;
}

bool Coordinator::try_place_locked(Group& g, int spares_reserve) {
    const PoolConfig* pool = cfg_.find_pool(g.pool);
    if (!pool) return false;
    Placement p = place_group(*pool, free_nodes_locked(), spares_reserve, cfg_.pinned, pool_model_files());
    if (!p.ok) {
        if (g.status != "unplaced" || g.error != p.reason) log::warn("group {}: unplaced: {}", g.group_id, p.reason);
        g.status = "unplaced";
        g.error = p.reason;
        return false;
    }
    assign_locked(g, p);
    return true;
}

void Coordinator::assign_locked(Group& g, const Placement& p) {
    g.head = p.head;
    g.workers = p.workers;
    const Node& h = nodes_.at(p.head);
    g.head_url = std::format("http://{}:{}", h.ip, h.head_port);
    nodes_.at(p.head).group_id = g.group_id;
    nodes_.at(p.head).role = "head";
    for (const auto& w : p.workers) {
        nodes_.at(w).group_id = g.group_id;
        nodes_.at(w).role = "worker";
    }
    g.repair_unplaced = false;
    log::info("group {}: placed head={} workers=[{}]", g.group_id, g.head, join(g.workers, ","));
}

void Coordinator::release_members_locked(Group& g) {
    for (const auto& ep : member_endpoints_locked(g, false)) {
        auto it = nodes_.find(ep.node_id);
        if (it != nodes_.end() && it->second.group_id == g.group_id) {
            it->second.group_id.clear();
            it->second.role.clear();
        }
    }
    g.head.clear();
    g.workers.clear();
    g.head_url.clear();
    g.expected.clear();
}

Coordinator::Endpoint Coordinator::endpoint_locked(const std::string& node_id) const {
    Endpoint e;
    e.node_id = node_id;
    if (auto it = nodes_.find(node_id); it != nodes_.end()) {
        e.ip = it->second.ip;
        e.agent_port = it->second.agent_port;
        e.rpc_port = it->second.rpc_port;
        e.head_port = it->second.head_port;
    }
    return e;
}

std::vector<Coordinator::Endpoint> Coordinator::member_endpoints_locked(const Group& g, bool alive_only) const {
    std::vector<std::string> ids;
    if (!g.head.empty()) ids.push_back(g.head);
    ids.insert(ids.end(), g.workers.begin(), g.workers.end());
    std::vector<Endpoint> out;
    for (const auto& id : ids) {
        auto it = nodes_.find(id);
        if (it == nodes_.end()) continue;
        if (alive_only && !it->second.alive) continue;
        out.push_back(endpoint_locked(id));
    }
    return out;
}

void Coordinator::mark_bad_locked(Group& g, const std::string& status, const std::string& reason) {
    log::warn("group {}: {} -> {}: {}", g.group_id, g.status, status, reason);
    g.status = status;
    g.error = reason;
    cancel_task_locked(g);
    // First repair is immediate; repeated failures back off (heartbeat x 2^n, capped).
    double delay = g.repair_attempts == 0
                       ? 0
                       : std::min(kMaxRepairBackoffS,
                                  cfg_.coordinator.heartbeat_interval_s * std::pow(2.0, g.repair_attempts - 1));
    g.next_repair_mono = mono_now() + delay;
    ++g.repair_attempts;
}

std::optional<std::string> Coordinator::member_failure_locked(const Group& g) const {
    for (const auto& [node_id, exp] : g.expected) {
        auto it = nodes_.find(node_id);
        if (it == nodes_.end()) return std::format("node {} disappeared", node_id);
        const Node& n = it->second;
        auto pit = n.processes.find(exp.role);
        ProcState p = pit == n.processes.end() ? ProcState{} : pit->second;
        if (p.state == "exited" && p.pid == exp.pid)
            return std::format("{} process on {} exited (code {})", exp.role, node_id,
                               p.exit_code ? std::to_string(*p.exit_code) : "?");
        bool fresh = n.last_seen_mono > exp.started_mono + kHeartbeatGraceS;
        if (fresh && (p.state != "running" || (exp.pid && p.pid != exp.pid)))
            return std::format("{} process on {} is {}", exp.role, node_id, p.state == "running" ? "a different pid" : p.state);
    }
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------ tasks

std::shared_ptr<Coordinator::Task> Coordinator::cancel_task_locked(Group& g) {
    g.generation = ++gen_counter_;  // any in-flight task for this group can no longer write
    auto it = tasks_.find(g.group_id);
    if (it == tasks_.end()) return nullptr;
    // The task stays in tasks_ until it finishes, so the next launch for this group waits for it.
    it->second->stop.request_stop();
    return it->second;
}

bool Coordinator::task_busy_locked(const std::string& gid) const {
    auto it = tasks_.find(gid);
    if (it == tasks_.end()) return false;
    const auto& t = it->second;
    return !t->stop.stop_requested() &&
           t->finished.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

void Coordinator::launch_locked(Group& g, TaskKind kind) {
    auto prev = cancel_task_locked(g);
    if (prev) retired_.push_back(prev);  // joined by reap_tasks() once finished
    std::uint64_t gen = g.generation;
    auto t = std::make_shared<Task>();
    std::promise<void> done;
    t->finished = done.get_future().share();
    std::string gid = g.group_id;
    t->thread = std::thread([this, gid, gen, kind, prev, st = t->stop.token(), done = std::move(done)]() mutable {
        if (prev) prev->finished.wait();  // never two tasks acting on one group at once
        try {
            run_task(gid, gen, kind, st);
        } catch (const std::exception& e) {
            log::error("group {}: task error: {}", gid, e.what());
            fail_task(gid, gen, std::string("internal error: ") + e.what());
        }
        done.set_value();
    });
    tasks_[gid] = t;
}

void Coordinator::fail_task(const std::string& gid, std::uint64_t gen, const std::string& reason) {
    std::lock_guard lk(mu_);
    if (!is_current_locked(gid, gen)) return;
    mark_bad_locked(*find_group_locked(gid), "failed", reason);
}

void Coordinator::stop_endpoints(const std::vector<Endpoint>& eps) {
    std::vector<std::future<HttpResult>> fs;
    for (const auto& ep : eps)
        fs.push_back(std::async(std::launch::async, [this, ep] {
            return http_call("POST", ep.agent_url(), "/stop", cfg_.cluster_token, json{{"role", "all"}}, kAgentStopTimeoutS);
        }));
    for (size_t i = 0; i < fs.size(); ++i) {
        auto r = fs[i].get();
        if (!r.ok) log::debug("stop on {} failed (ignored): {}", eps[i].node_id, r.error);
    }
}

void Coordinator::run_task(const std::string& gid, std::uint64_t gen, TaskKind kind, StopToken st) {
    if (kind == TaskKind::restart || kind == TaskKind::repair) {
        std::vector<Endpoint> members;
        {
            std::lock_guard lk(mu_);
            if (!is_current_locked(gid, gen)) return;
            members = member_endpoints_locked(*find_group_locked(gid), /*alive_only=*/true);
        }
        // 03 §6 repair step 1: stop all alive members.
        stop_endpoints(members);
        if (st.stop_requested()) return;
        if (kind == TaskKind::repair) {
            std::lock_guard lk(mu_);
            if (!is_current_locked(gid, gen)) return;
            Group& g = *find_group_locked(gid);
            std::string why = g.error.value_or("repair");
            release_members_locked(g);  // step 2: survivors go back to free too
            // Step 3: re-place this one group; spares are usable now.
            if (!try_place_locked(g, 0)) {
                g.repair_unplaced = true;
                return;
            }
            log::info("group {}: repairing after: {}", gid, why);
        }
    }
    start_sequence(gid, gen, st);
}

void Coordinator::start_sequence(const std::string& gid, std::uint64_t gen, StopToken st) {
    std::vector<Endpoint> workers;
    Endpoint head;
    PoolConfig pool;
    std::string head_url;
    int slots = 1;
    {
        std::lock_guard lk(mu_);
        if (!is_current_locked(gid, gen)) return;
        Group& g = *find_group_locked(gid);
        const PoolConfig* p = cfg_.find_pool(g.pool);
        if (!p || g.head.empty()) return;
        pool = *p;
        g.status = "starting";
        g.started_at = wall_now();
        g.healthy_at.reset();
        g.health_failures = 0;  // `error` keeps the last failure until the group is healthy again
        g.expected.clear();
        head = endpoint_locked(g.head);
        for (const auto& w : g.workers) workers.push_back(endpoint_locked(w));
        head_url = g.head_url;
        slots = g.slots;
    }
    log::info("group {}: starting {} worker(s), head {}", gid, workers.size(), head.node_id);

    // 1-2. Start every worker's rpc-server in parallel.
    std::vector<std::future<HttpResult>> fs;
    json rpc_body{{"threads", nullptr}, {"cache", cfg_.llama_cpp.rpc_cache}};
    for (const auto& w : workers)
        fs.push_back(std::async(std::launch::async, [this, w, rpc_body] {
            return http_call("POST", w.agent_url(), "/rpc/start", cfg_.cluster_token, rpc_body, kAgentStartTimeoutS);
        }));
    std::vector<std::optional<long long>> worker_pids;
    std::string rpc_error;
    for (size_t i = 0; i < fs.size(); ++i) {
        auto r = fs[i].get();
        if (!r.ok && rpc_error.empty()) rpc_error = std::format("rpc/start on {} failed: {}", workers[i].node_id, r.error);
        worker_pids.push_back(json_ll(r.body, "pid"));
    }
    if (st.stop_requested()) return;
    if (!rpc_error.empty()) return fail_task(gid, gen, rpc_error);

    // 3. Wait until each worker's RPC port accepts connections.
    for (const auto& w : workers) {
        double deadline = mono_now() + kRpcPortTimeoutS;
        while (!tcp_probe(w.ip, w.rpc_port, 1000)) {
            if (mono_now() > deadline)
                return fail_task(gid, gen, std::format("rpc-server on {} ({}:{}) did not open within {} s", w.node_id,
                                                       w.ip, w.rpc_port, kRpcPortTimeoutS));
            if (!interruptible_sleep(st, 1.0)) return;
        }
    }

    // 4. Start the head.
    std::vector<std::string> endpoints;
    for (const auto& w : workers) endpoints.push_back(std::format("{}:{}", w.ip, w.rpc_port));
    std::vector<std::string> extra = pool.extra_args;
    extra.insert(extra.end(), cfg_.llama_cpp.extra_head_args.begin(), cfg_.llama_cpp.extra_head_args.end());
    json head_body{{"model_file", pool.model_file},
                   {"rpc_endpoints", endpoints},
                   {"context", pool.context},
                   {"parallel", slots},
                   {"threads", nullptr},
                   {"ngl", 99},
                   {"tensor_split", pool.tensor_split ? json(*pool.tensor_split) : json(nullptr)},
                   {"extra_args", extra}};
    {
        std::lock_guard lk(mu_);
        if (!is_current_locked(gid, gen)) return;
        Group& g = *find_group_locked(gid);
        g.status = "loading";
        double now = mono_now();
        for (size_t i = 0; i < workers.size(); ++i) g.expected[workers[i].node_id] = {"rpc", worker_pids[i], now};
    }
    log::info("group {}: loading {} on {}", gid, pool.model_file, head.node_id);
    auto hr = http_call("POST", head.agent_url(), "/head/start", cfg_.cluster_token, head_body, kAgentStartTimeoutS);
    if (st.stop_requested()) return;
    if (!hr.ok) return fail_task(gid, gen, std::format("head/start on {} failed: {}", head.node_id, hr.error));
    {
        std::lock_guard lk(mu_);
        if (!is_current_locked(gid, gen)) return;
        find_group_locked(gid)->expected[head.node_id] = {"head", json_ll(hr.body, "pid"), mono_now()};
    }

    // 5. Poll /health until 200. 503 = still loading (can take an hour on first deploy).
    //    A head that exits is caught by the monitor, which cancels this task.
    double poll = std::min(5.0, cfg_.coordinator.heartbeat_interval_s);
    double deadline = mono_now() + cfg_.coordinator.group_start_timeout_s;
    while (true) {
        if (!interruptible_sleep(st, poll)) return;
        auto h = http_call("GET", head_url, "/health", "", nullptr, 5);
        if (h.status == 200) break;
        {
            std::lock_guard lk(mu_);
            if (!is_current_locked(gid, gen)) return;
        }
        if (mono_now() > deadline)
            return fail_task(gid, gen, std::format("head did not become healthy within {} s", cfg_.coordinator.group_start_timeout_s));
    }

    // 6. Healthy.
    std::lock_guard lk(mu_);
    if (!is_current_locked(gid, gen)) return;
    Group& g = *find_group_locked(gid);
    g.status = "healthy";
    g.healthy_at = wall_now();
    g.error.reset();
    g.health_failures = 0;
    g.repair_attempts = 0;
    log::info("group {}: healthy at {} ({:.1f} s)", gid, g.head_url, *g.healthy_at - g.started_at.value_or(*g.healthy_at));
}

int Coordinator::undeploy_all() {
    std::vector<std::shared_ptr<Task>> cancelled;
    std::vector<Endpoint> members;
    int stopped = 0;
    {
        std::lock_guard lk(mu_);
        for (auto& g : groups_) {
            if (auto t = cancel_task_locked(g)) cancelled.push_back(t);
            auto eps = member_endpoints_locked(g, true);
            members.insert(members.end(), eps.begin(), eps.end());
            release_members_locked(g);
            if (g.status != "stopped") ++stopped;
            g.status = "stopped";
            g.error.reset();
            g.healthy_at.reset();
        }
        deployed_ = false;
    }
    // Let cancelled tasks finish their in-flight call so none lands after our stop.
    for (auto& t : cancelled) t->finished.wait();
    stop_endpoints(members);
    log::info("undeploy: {} group(s) stopped", stopped);
    return stopped;
}

// ------------------------------------------------------------------------------------------ monitor

void Coordinator::start() {
    monitor_ = std::thread([this, st = monitor_stop_.token()] { monitor_loop(st); });
}

void Coordinator::stop() {
    if (monitor_.joinable()) {
        monitor_stop_.request_stop();
        monitor_.join();
    }
    std::vector<std::shared_ptr<Task>> all;
    {
        std::lock_guard lk(mu_);
        for (auto& g : groups_) cancel_task_locked(g);
        for (auto& [_, t] : tasks_) {
            t->stop.request_stop();
            all.push_back(t);
        }
        tasks_.clear();
        all.insert(all.end(), retired_.begin(), retired_.end());
        retired_.clear();
    }
    for (auto& t : all)
        if (t->thread.joinable()) t->thread.join();
}

void Coordinator::monitor_loop(StopToken st) {
    while (interruptible_sleep(st, cfg_.coordinator.heartbeat_interval_s)) {
        try {
            monitor_cycle();
            reap_tasks();
        } catch (const std::exception& e) {
            log::error("monitor: {}", e.what());
        }
    }
}

void Coordinator::reap_tasks() {
    std::vector<std::shared_ptr<Task>> done;
    {
        std::lock_guard lk(mu_);
        auto ready = [](const std::shared_ptr<Task>& t) {
            return t->finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        };
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (ready(*it)) {
                done.push_back(*it);
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = tasks_.begin(); it != tasks_.end();) {
            if (ready(it->second)) {
                done.push_back(it->second);
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& t : done)
        if (t->thread.joinable()) t->thread.join();
}

void Coordinator::monitor_cycle() {
    struct Probe {
        std::string gid, url;
        std::uint64_t gen;
    };
    std::vector<Probe> probes;
    {
        std::lock_guard lk(mu_);
        double now = mono_now();

        // Node liveness.
        for (auto& [id, n] : nodes_) {
            if (!n.alive || now - n.last_seen_mono <= cfg_.coordinator.node_timeout_s) continue;
            n.alive = false;
            log::warn("node {} is dead (no heartbeat for {:.0f} s)", id, now - n.last_seen_mono);
            if (Group* g = n.group_id.empty() ? nullptr : find_group_locked(n.group_id)) {
                if (g->status == "starting" || g->status == "loading" || g->status == "healthy")
                    mark_bad_locked(*g, "degraded", std::format("member {} is dead", id));
            }
        }

        for (auto& g : groups_) {
            // Process exits reported by heartbeats.
            if (g.status == "loading" || g.status == "healthy") {
                if (auto why = member_failure_locked(g))
                    mark_bad_locked(g, g.status == "healthy" ? "degraded" : "failed", *why);
            }
            // Repairs (one task per group).
            bool busy = task_busy_locked(g.group_id);
            if ((g.status == "degraded" || g.status == "failed") && !busy && now >= g.next_repair_mono)
                launch_locked(g, TaskKind::repair);
            // Unplaced retry.
            if (deployed_ && g.status == "unplaced" && !busy) {
                if (try_place_locked(g, g.repair_unplaced ? 0 : cfg_.spares)) launch_locked(g, TaskKind::start);
            }
            if (g.status == "healthy") probes.push_back({g.group_id, g.head_url, g.generation});
        }
    }

    // Head health, in parallel, outside the lock.
    double timeout = std::min(5.0, cfg_.coordinator.heartbeat_interval_s);
    std::vector<std::future<bool>> fs;
    for (const auto& p : probes)
        fs.push_back(std::async(std::launch::async, [url = p.url, timeout] {
            return http_call("GET", url, "/health", "", nullptr, timeout).status == 200;
        }));
    std::lock_guard lk(mu_);
    for (size_t i = 0; i < probes.size(); ++i) {
        bool ok = fs[i].get();
        Group* g = find_group_locked(probes[i].gid);
        if (!g || g->generation != probes[i].gen || g->status != "healthy") continue;
        if (ok) {
            g->health_failures = 0;
        } else if (++g->health_failures >= kHealthFailLimit) {
            mark_bad_locked(*g, "degraded", std::format("head /health failed {} times in a row", g->health_failures));
        }
    }
}

}  // namespace xgi
