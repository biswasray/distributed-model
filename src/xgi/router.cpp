#include "xgi/router.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <memory>

#include "common/http_util.hpp"
#include "common/log.hpp"
#include "common/util.hpp"
#include "xgi/scheduling.hpp"

namespace xgi {

namespace {

// One proxied request to a head. The HTTP client runs on its own thread and hands
// headers and body chunks to the server thread through this object.
struct Upstream {
    std::mutex mu;
    std::condition_variable cv;
    bool have_headers = false;
    bool done = false;
    int status = 0;
    std::string content_type;
    std::deque<std::string> chunks;
    std::string error;  // transport error (empty = clean finish)
    std::atomic<bool> cancelled{false};
    std::unique_ptr<httplib::Client> client;
    std::thread thread;

    void cancel() {
        if (!cancelled.exchange(true) && client) client->stop();  // aborts a blocked read at once
    }
    void join() {
        if (thread.joinable()) thread.join();
    }
};

std::shared_ptr<Upstream> start_upstream(const BaseUrl& url, const std::string& path, const std::string& body,
                                         const std::string& accept, double timeout_s) {
    auto up = std::make_shared<Upstream>();
    up->client = std::make_unique<httplib::Client>(url.host, url.port);
    up->client->set_connection_timeout(10, 0);
    up->client->set_read_timeout(static_cast<time_t>(timeout_s), 0);
    up->client->set_write_timeout(60, 0);
    up->client->set_keep_alive(false);
    up->thread = std::thread([up, path, body, accept, timeout_s] {
        double deadline = mono_now() + timeout_s;
        httplib::Request r;
        r.method = "POST";
        r.path = path;
        r.body = body;
        r.set_header("Content-Type", "application/json");
        r.set_header("Accept", accept.empty() ? "*/*" : accept);
        r.response_handler = [&](const httplib::Response& resp) {
            std::lock_guard lk(up->mu);
            up->have_headers = true;
            up->status = resp.status;
            up->content_type = resp.get_header_value("Content-Type");
            up->cv.notify_all();
            return !up->cancelled.load();
        };
        r.content_receiver = [&](const char* data, size_t n, uint64_t, uint64_t) {
            if (up->cancelled.load()) return false;
            if (mono_now() > deadline) {
                std::lock_guard lk(up->mu);
                up->error = "request_timeout_s exceeded";
                return false;
            }
            std::lock_guard lk(up->mu);
            up->chunks.emplace_back(data, n);
            up->cv.notify_all();
            return true;
        };
        auto result = up->client->send(r);
        std::lock_guard lk(up->mu);
        if (!result && !up->cancelled.load() && up->error.empty()) up->error = httplib::to_string(result.error());
        up->done = true;
        up->cv.notify_all();
    });
    return up;
}

bool is_event_stream(const std::string& ctype) { return to_lower(ctype).find("text/event-stream") != std::string::npos; }

}  // namespace

Router::Router(ClusterConfig cfg) : cfg_(std::move(cfg)) {
    for (const auto& p : cfg_.pools) pools_[p.name].configured = true;
}

Router::~Router() { stop(); }

size_t Router::recommended_threads() const {
    size_t slots = 0;
    for (const auto& p : cfg_.pools) slots += static_cast<size_t>(p.groups) * static_cast<size_t>(cfg_.slots_for(p));
    size_t queued = cfg_.pools.size() * static_cast<size_t>(cfg_.router.max_queue);
    return std::clamp<size_t>(slots + queued + 32, 64, 2048);
}

// ------------------------------------------------------------------------------------------ cluster view

void Router::update_table(const json& groups) {
    if (!groups.is_array()) return;
    double now = mono_now();
    std::lock_guard lk(mu_);
    std::map<std::string, std::vector<RGroup>> next;
    std::map<std::string, RGroup> old;
    for (auto& [name, ps] : pools_)
        for (auto& g : ps.groups) old[name + "\n" + g.group_id] = g;

    for (const auto& j : groups) {
        if (!j.is_object() || !j["pool"].is_string() || !j["group_id"].is_string()) continue;
        std::string pool = j["pool"], gid = j["group_id"];
        RGroup g;
        if (auto it = old.find(pool + "\n" + gid); it != old.end()) {
            g = it->second;
            old.erase(it);
        } else {
            g.group_id = gid;
            g.idle_since = now;
        }
        g.head_url = j["head_url"].is_string() ? j["head_url"].get<std::string>() : "";
        g.slots = j["slots"].is_number_integer() ? std::max(1, j["slots"].get<int>()) : cfg_.router.slots_per_group;
        g.healthy = j.value("status", "") == "healthy" && !g.head_url.empty();
        next[pool].push_back(std::move(g));
    }
    // Groups that vanished but still have requests in flight stay (unroutable) until released.
    for (auto& [key, g] : old) {
        if (g.inflight == 0) continue;
        g.healthy = false;
        next[key.substr(0, key.find('\n'))].push_back(g);
    }
    for (auto& [name, ps] : pools_) ps.groups.clear();
    for (auto& [name, gs] : next) pools_[name].groups = std::move(gs);
    cv_.notify_all();
}

void Router::poll_loop(std::stop_token st) {
    bool warned = false;
    do {
        auto r = http_call("GET", cfg_.router.coordinator_url, "/groups", cfg_.cluster_token, nullptr, 5);
        if (r.ok && r.body.is_array()) {
            update_table(r.body);
            {
                std::lock_guard lk(mu_);
                last_poll_ok_mono_ = mono_now();
            }
            if (warned) log::info("coordinator reachable again");
            warned = false;
        } else if (!warned) {
            // 04 §2: keep using the last known table; heads keep serving.
            log::warn("cannot poll {}/groups ({}); using last known table", cfg_.router.coordinator_url, r.error);
            warned = true;
        }
    } while (interruptible_sleep(st, kPollIntervalS));
}

void Router::start() {
    poller_ = std::jthread([this](std::stop_token st) { poll_loop(st); });
}

void Router::stop() {
    {
        std::lock_guard lk(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (poller_.joinable()) {
        poller_.request_stop();
        poller_.join();
    }
}

// ------------------------------------------------------------------------------------------ admission

Router::Admission Router::acquire(const std::string& pool, const std::string& exclude) {
    Admission a;
    double t0 = mono_now();
    std::unique_lock lk(mu_);
    auto it = pools_.find(pool);
    if (it == pools_.end() || (!it->second.configured && it->second.groups.empty())) {
        a.status = Admit::unknown_model;
        return a;
    }
    PoolState& ps = it->second;
    auto loads = [&] {
        std::vector<GroupLoad> v;
        for (const auto& g : ps.groups) v.push_back({g.group_id, g.inflight, g.slots, g.idle_since, g.suspect_until, g.healthy});
        return v;
    };
    auto take = [&](size_t idx) {
        RGroup& g = ps.groups[idx];
        ++g.inflight;
        a.status = Admit::ok;
        a.group_id = g.group_id;
        a.head_url = g.head_url;
        a.queue_ms = (mono_now() - t0) * 1000.0;
    };

    if (!any_routable(loads(), mono_now(), exclude)) {
        a.status = Admit::no_healthy;
        return a;
    }
    if (ps.queue.empty()) {
        if (auto idx = select_group(loads(), mono_now(), exclude)) {
            take(*idx);
            return a;
        }
    }
    if (static_cast<int>(ps.queue.size()) >= cfg_.router.max_queue) {
        a.status = Admit::queue_full;
        return a;
    }

    // All busy: wait FIFO, per pool, up to queue_timeout_s.
    std::uint64_t ticket = next_ticket_++;
    ps.queue.push_back(ticket);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                           std::chrono::duration<double>(cfg_.router.queue_timeout_s));
    auto try_front = [&] {
        if (stopping_ || ps.queue.empty() || ps.queue.front() != ticket) return false;
        auto idx = select_group(loads(), mono_now(), exclude);
        if (!idx) return false;
        ps.queue.pop_front();
        take(*idx);
        cv_.notify_all();  // the next waiter may fit too
        return true;
    };
    while (true) {
        if (try_front()) return a;
        bool timed_out = cv_.wait_until(lk, deadline) == std::cv_status::timeout;
        if (try_front()) return a;
        if (stopping_ || timed_out) {
            std::erase(ps.queue, ticket);
            cv_.notify_all();
            a.status = Admit::timeout;
            a.queue_ms = (mono_now() - t0) * 1000.0;
            return a;
        }
    }
}

void Router::release(const std::string& pool, const std::string& group_id) {
    std::lock_guard lk(mu_);
    if (auto it = pools_.find(pool); it != pools_.end()) {
        for (auto& g : it->second.groups) {
            if (g.group_id != group_id) continue;
            if (g.inflight > 0 && --g.inflight == 0) g.idle_since = mono_now();
            break;
        }
    }
    cv_.notify_all();
}

void Router::mark_suspect(const std::string& pool, const std::string& group_id) {
    std::lock_guard lk(mu_);
    if (auto it = pools_.find(pool); it != pools_.end())
        for (auto& g : it->second.groups)
            if (g.group_id == group_id) g.suspect_until = mono_now() + kSuspectS;
}

// ------------------------------------------------------------------------------------------ HTTP

bool Router::authorized(const httplib::Request& req, httplib::Response& res) const {
    if (cfg_.router.api_keys.empty()) return true;
    std::string auth = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.size() > prefix.size() && to_lower(auth.substr(0, prefix.size())) == "bearer ") {
        std::string key = auth.substr(prefix.size());
        for (const auto& k : cfg_.router.api_keys)
            if (secure_equals(key, k)) return true;
    }
    reply_error(res, 401, "missing or invalid API key", "auth");
    return false;
}

json Router::health_json() {
    std::lock_guard lk(mu_);
    json pools = json::object();
    for (const auto& [name, ps] : pools_) {
        int healthy = 0, inflight = 0, slots = 0;
        for (const auto& g : ps.groups) {
            if (g.healthy) {
                ++healthy;
                slots += g.slots;
            }
            inflight += g.inflight;
        }
        pools[name] = json{{"healthy_groups", healthy}, {"slots", slots}, {"inflight", inflight}, {"queued", ps.queue.size()}};
    }
    double age = last_poll_ok_mono_ > 0 ? mono_now() - last_poll_ok_mono_ : -1;
    return json{{"status", "ok"},
                {"service", "xgi-router"},
                {"version", kVersion},
                {"coordinator_poll_age_s", age < 0 ? json(nullptr) : json(age)},
                {"pools", pools}};
}

void Router::register_routes(httplib::Server& s) {
    s.Get("/health", [this](const httplib::Request&, httplib::Response& res) { reply_json(res, 200, health_json()); });

    s.Get("/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) return;
        json data = json::array();
        std::lock_guard lk(mu_);
        for (const auto& [name, ps] : pools_) {
            bool any = std::any_of(ps.groups.begin(), ps.groups.end(), [](const RGroup& g) { return g.healthy; });
            if (any) data.push_back(json{{"id", name}, {"object", "model"}, {"created", 0}, {"owned_by", "qwen-cluster"}});
        }
        reply_json(res, 200, json{{"object", "list"}, {"data", data}});
    });

    auto proxy = [this](const httplib::Request& req, httplib::Response& res) { handle_proxy(req, res); };
    s.Post("/v1/chat/completions", proxy);
    s.Post("/v1/completions", proxy);
}

void Router::handle_proxy(const httplib::Request& req, httplib::Response& res) {
    if (!authorized(req, res)) return;
    auto body = parse_body(req, res);
    if (!body) return;
    if (!body->is_object() || !(*body)["model"].is_string())
        return reply_error(res, 400, "'model' is required (a pool name)", "invalid_request");
    const std::string pool = (*body)["model"];

    std::string exclude;
    for (int attempt = 0; attempt < 2; ++attempt) {
        Admission a = acquire(pool, exclude);
        switch (a.status) {
            case Admit::ok: break;
            case Admit::unknown_model:
            case Admit::no_healthy:
                if (attempt > 0) return reply_error(res, 502, "the head failed and no other group is available", "upstream");
                return reply_error(res, 503, "model unavailable", "capacity");
            case Admit::queue_full:
                res.set_header("Retry-After", "5");
                return reply_error(res, 429, "queue full", "capacity");
            case Admit::timeout:
                return reply_error(res, 503, "timed out waiting for capacity", "capacity");
        }

        auto url = parse_base_url(a.head_url);
        if (!url) {
            release(pool, a.group_id);
            mark_suspect(pool, a.group_id);
            exclude = a.group_id;
            continue;
        }
        auto up = start_upstream(*url, req.path, req.body, req.get_header_value("Accept"), cfg_.router.request_timeout_s);
        {
            std::unique_lock lk(up->mu);
            up->cv.wait(lk, [&] { return up->have_headers || up->done; });
        }
        if (!up->have_headers) {
            // 04 §4.3: connection error before any response bytes -> suspect for 30 s, retry once elsewhere.
            up->join();
            log::warn("{} -> {} ({}): {}; retrying on another group", pool, a.group_id, a.head_url, up->error);
            release(pool, a.group_id);
            mark_suspect(pool, a.group_id);
            exclude = a.group_id;
            if (attempt == 0) continue;
            return reply_error(res, 502, "the head failed and the retry failed too: " + up->error, "upstream");
        }

        res.status = up->status;
        res.set_header("X-Qwen-Pool", pool);
        res.set_header("X-Qwen-Group", a.group_id);
        res.set_header("X-Qwen-Queue-Ms", std::to_string(static_cast<long long>(a.queue_ms)));
        log::debug("{} -> {} status {} (queued {:.0f} ms)", pool, a.group_id, up->status, a.queue_ms);

        if (is_event_stream(up->content_type)) {
            // Relay SSE bytes as they arrive. A client that goes away cancels the upstream request,
            // so llama-server frees the slot.
            std::string gid = a.group_id;
            res.set_chunked_content_provider(
                up->content_type,
                [up](size_t, httplib::DataSink& sink) {
                    std::unique_lock lk(up->mu);
                    while (true) {
                        if (!up->chunks.empty()) {
                            std::string c = std::move(up->chunks.front());
                            up->chunks.pop_front();
                            lk.unlock();
                            if (!sink.write(c.data(), c.size())) {
                                up->cancel();
                                return false;
                            }
                            lk.lock();
                            continue;
                        }
                        if (up->done) {
                            std::string err = up->error;
                            lk.unlock();
                            if (!err.empty()) {  // failed after streaming began: no retry, end with an SSE error
                                std::string ev = "data: " + error_body(502, "upstream failed: " + err, "upstream").dump() + "\n\n";
                                sink.write(ev.data(), ev.size());
                            }
                            sink.done();
                            return true;
                        }
                        up->cv.wait_for(lk, std::chrono::seconds(1));
                        if (up->chunks.empty() && !up->done && !sink.is_writable()) {
                            lk.unlock();
                            up->cancel();
                            return false;
                        }
                    }
                },
                [this, up, pool, gid](bool) {
                    up->cancel();
                    up->join();
                    release(pool, gid);
                });
            return;
        }

        // Non-streaming: llama-server sends the whole JSON at the end.
        {
            std::unique_lock lk(up->mu);
            up->cv.wait(lk, [&] { return up->done; });
        }
        up->join();
        release(pool, a.group_id);
        if (!up->error.empty()) return reply_error(res, 502, "upstream failed: " + up->error, "upstream");
        std::string out;
        for (auto& c : up->chunks) out += c;
        res.set_content(out, up->content_type.empty() ? "application/json" : up->content_type);
        return;
    }
    reply_error(res, 502, "no usable group", "upstream");
}

}  // namespace xgi
