// XI: the node agent (architecture/02-node-agent.md).
#pragma once

#include <httplib.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/util.hpp"
#include "xi/process.hpp"
#include "xi/runtimes.hpp"
#include "xi/settings.hpp"

namespace xi {

// How this XI process was started (built by main from flags, xi.yaml and the environment).
struct AgentBoot {
    AgentSettings settings;            // defaults < xi.yaml < flags
    AgentSettings file_settings;       // what xi.yaml says (defaults if there is no file)
    SettingsLocation file;             // xi.yaml: where it is, or where a save creates it
    std::set<std::string> cli_keys;    // settings given as flags: they win again at the next start
    std::string token;
    std::string token_source = "none"; // flag | env | none
    int exec_max_jobs = 2;             // concurrent /exec jobs
};

class Agent {
public:
    explicit Agent(AgentBoot boot);
    ~Agent();

    // Runs the HTTP API and the register/heartbeat loop until stop(). Returns false if the port can't be bound.
    bool run();
    // Called from the shutdown handler: stops the children, then makes run() return.
    void request_shutdown();
    // Stops everything, including child processes. Call from the thread that called run().
    void stop();

    nlohmann::json facts();      // the /register body and GET /status
    nlohmann::json processes();  // {"rpc": ProcInfo, "head": ProcInfo}

private:
    // Values derived from the settings (defaults filled in).
    struct Resolved {
        std::string name, advertise_ip;
        std::filesystem::path models_dir, exec_dir, rpc_server_bin, llama_server_bin;
    };
    struct Snapshot {
        AgentSettings s;
        Resolved r;
        std::string token;
    };
    struct Link {  // registration with the coordinator, for the web page
        std::string state = "starting";  // not_configured | registering | registered | error
        std::string coordinator, node_id, last_error;
        double registered_at = 0, last_heartbeat_ok = 0;
    };

    Snapshot snapshot() const;
    static Resolved resolve(const AgentSettings& s);
    nlohmann::json facts(const Snapshot& snap);
    std::vector<Runtime> runtimes() const;
    void set_link(const std::function<void(Link&)>& fn);

    void setup_routes();
    void setup_ui_routes();
    void on_get_settings(const httplib::Request&, httplib::Response&);
    void on_put_settings(const httplib::Request&, httplib::Response&);
    void control_loop();
    bool wait_or_wake(double seconds);  // true if stopping
    bool do_register(const Snapshot& snap);
    int heartbeat(const Snapshot& snap);  // HTTP status, 0 = network error
    int default_threads() const;

    // Settings: `running_` is in effect; `configured_` is what xi.yaml holds (it differs only in
    // restart keys, until the next start).
    mutable std::mutex cfg_mu_;
    AgentSettings running_;
    AgentSettings configured_;
    Resolved resolved_;
    std::string token_;
    std::string token_source_;
    SettingsLocation file_;
    std::set<std::string> cli_keys_;
    int exec_max_jobs_ = 2;
    const double started_wall_;

    mutable std::mutex link_mu_;
    Link link_;

    httplib::Server server_;
    std::unique_ptr<ManagedProcess> rpc_;
    std::unique_ptr<ManagedProcess> head_;
    std::mutex start_mu_;  // serialises start/stop requests
    xgi::StopSource stop_flag_;
    std::atomic<bool> reregister_{false};
    std::thread control_thread_;
    std::atomic<bool> stopped_{false};

    mutable std::mutex rt_mu_;
    std::vector<Runtime> runtimes_;
    std::mutex detect_mu_;  // one detection at a time
    std::atomic<int> exec_jobs_{0};
};

}  // namespace xi
