// XGI router: OpenAI-compatible gateway (architecture/04-router.md).
#pragma once

#include <httplib.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "common/util.hpp"

namespace xgi {

class Router {
public:
    static constexpr double kPollIntervalS = 3;   // 04 §2
    static constexpr double kSuspectS = 30;       // 04 §4.3

    explicit Router(ClusterConfig cfg);
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    void register_routes(httplib::Server& server);
    // Thread pool size the HTTP server needs: queued requests each hold a thread while waiting.
    size_t recommended_threads() const;
    void start();  // starts polling the coordinator
    void stop();

    // Replaces the cluster view from a GET /groups body (public for tests).
    void update_table(const nlohmann::json& groups);

    enum class Admit { ok, unknown_model, no_healthy, queue_full, timeout };
    struct Admission {
        Admit status = Admit::unknown_model;
        std::string group_id, head_url;
        double queue_ms = 0;
    };
    // Waits for a slot in `pool` (FIFO), skipping `exclude`. On ok the caller must release().
    Admission acquire(const std::string& pool, const std::string& exclude = "");
    void release(const std::string& pool, const std::string& group_id);
    void mark_suspect(const std::string& pool, const std::string& group_id);

    nlohmann::json health_json();

private:
    struct RGroup {
        std::string group_id, head_url;
        int slots = 1;
        int inflight = 0;
        double idle_since = 0;
        double suspect_until = 0;
        bool healthy = false;
    };
    struct PoolState {
        bool configured = false;
        std::vector<RGroup> groups;
        std::deque<std::uint64_t> queue;
    };

    bool authorized(const httplib::Request& req, httplib::Response& res) const;
    void handle_proxy(const httplib::Request& req, httplib::Response& res);
    void poll_loop(StopToken st);

    ClusterConfig cfg_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, PoolState> pools_;
    std::uint64_t next_ticket_ = 1;
    bool stopping_ = false;
    double last_poll_ok_mono_ = 0;
    std::thread poller_;
    StopSource poller_stop_;
};

}  // namespace xgi
