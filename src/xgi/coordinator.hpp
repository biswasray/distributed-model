// XGI coordinator: node registry, placement, group lifecycle and failover
// (architecture/03-coordinator.md). All state is in memory (ADR-006).
#pragma once

#include <httplib.h>

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "xgi/placement.hpp"

namespace xgi {

class Coordinator {
public:
    explicit Coordinator(ClusterConfig cfg);
    ~Coordinator();
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    void register_routes(httplib::Server& server);
    void start();  // starts the monitor loop
    void stop();   // stops the monitor and lifecycle tasks (llama.cpp processes keep running)

    nlohmann::json nodes_json();
    nlohmann::json groups_json();

private:
    struct ProcState {
        std::string state = "stopped";
        std::optional<long long> pid;
        std::optional<long long> exit_code;
    };
    struct Node {
        nlohmann::json facts;  // last /register body, updated by heartbeats
        std::string node_id, hostname, ip;
        int agent_port = 8200, rpc_port = 50052, head_port = 8080;
        std::uint64_t ram_available = 0;
        std::set<std::string> models;
        std::map<std::string, ProcState> processes;
        double last_seen = 0;       // wall clock
        double last_seen_mono = 0;  // monotonic
        bool alive = true;
        bool draining = false;
        std::string group_id;  // empty = free
        std::string role;      // head | worker | empty
    };
    struct Expected {  // a process this coordinator started on a group member
        std::string role;
        std::optional<long long> pid;
        double started_mono = 0;
    };
    struct Group {
        std::string group_id, pool;
        std::string head;
        std::vector<std::string> workers;
        std::string status = "unplaced";
        std::string head_url;
        int slots = 1;
        std::optional<std::string> error;
        std::optional<double> started_at, healthy_at;
        // internal
        std::map<std::string, Expected> expected;  // node_id -> process
        std::uint64_t generation = 0;              // bumps whenever the current task is replaced/cancelled
        int health_failures = 0;
        int repair_attempts = 0;
        double next_repair_mono = 0;
        bool repair_unplaced = false;  // became unplaced during repair: retry may use spares
    };
    struct Task {
        std::stop_source stop;
        std::thread thread;
        std::shared_future<void> finished;
    };
    enum class TaskKind { start, restart, repair };
    struct Endpoint {
        std::string node_id, ip;
        int agent_port = 0, rpc_port = 0, head_port = 0;
        std::string agent_url() const;
    };

    // --- HTTP handlers
    void on_register(const httplib::Request&, httplib::Response&);
    void on_heartbeat(const httplib::Request&, httplib::Response&);
    void on_deploy(const httplib::Request&, httplib::Response&);
    void on_undeploy(const httplib::Request&, httplib::Response&);
    void on_restart(const httplib::Request&, httplib::Response&);
    void on_drain(const httplib::Request&, httplib::Response&, bool drain);

    // --- state helpers (caller holds mu_)
    Group* find_group_locked(const std::string& id);
    bool is_current_locked(const std::string& gid, std::uint64_t gen);
    std::vector<FreeNode> free_nodes_locked() const;
    std::set<std::string> pool_model_files() const;
    bool try_place_locked(Group& g, int spares_reserve);
    void assign_locked(Group& g, const Placement& p);
    void release_members_locked(Group& g);
    std::vector<Endpoint> member_endpoints_locked(const Group& g, bool alive_only) const;
    Endpoint endpoint_locked(const std::string& node_id) const;
    void mark_bad_locked(Group& g, const std::string& status, const std::string& reason);
    std::optional<std::string> member_failure_locked(const Group& g) const;
    nlohmann::json node_json_locked(const Node& n) const;
    nlohmann::json group_json_locked(const Group& g) const;

    // --- lifecycle tasks
    void launch_locked(Group& g, TaskKind kind);
    std::shared_ptr<Task> cancel_task_locked(Group& g);
    bool task_busy_locked(const std::string& gid) const;
    void run_task(const std::string& gid, std::uint64_t gen, TaskKind kind, std::stop_token st);
    void start_sequence(const std::string& gid, std::uint64_t gen, std::stop_token st);
    void fail_task(const std::string& gid, std::uint64_t gen, const std::string& reason);
    void stop_endpoints(const std::vector<Endpoint>& eps);
    int undeploy_all();

    // --- monitor
    void monitor_loop(std::stop_token st);
    void monitor_cycle();
    void reap_tasks();

    ClusterConfig cfg_;
    std::mutex mu_;  // protects everything below; never held during network calls
    std::map<std::string, Node> nodes_;
    std::vector<Group> groups_;  // deploy order = placement priority
    bool deployed_ = false;
    std::uint64_t gen_counter_ = 0;
    std::map<std::string, std::shared_ptr<Task>> tasks_;
    std::vector<std::shared_ptr<Task>> retired_;
    std::mutex deploy_mu_;  // serialises deploy/undeploy
    std::jthread monitor_;
};

}  // namespace xgi
