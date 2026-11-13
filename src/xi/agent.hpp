// XI: the node agent (architecture/02-node-agent.md).
#pragma once

#include <httplib.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "common/util.hpp"
#include "xi/process.hpp"

namespace xi {

struct AgentOptions {
    std::string coordinator;  // base URL, required
    std::string token;
    std::string host = "0.0.0.0";
    int port = 8200;
    std::string name;          // node_id; default hostname
    std::string advertise_ip;  // default: route to coordinator
    std::filesystem::path llama_dir = ".\\llama.cpp";
    std::filesystem::path models_dir = ".\\models";
    std::filesystem::path log_dir = ".\\logs";
    int rpc_port = 50052;
    int head_port = 8080;
    std::filesystem::path rpc_server_bin;    // default <llama_dir>\rpc-server.exe
    std::filesystem::path llama_server_bin;  // default <llama_dir>\llama-server.exe
    double heartbeat_s = 5;
};

class Agent {
public:
    explicit Agent(AgentOptions opts);
    ~Agent();

    // Runs the HTTP API and the register/heartbeat loop until stop(). Returns false if the port can't be bound.
    bool run();
    // Safe from a console-control handler: makes run() return.
    void request_shutdown();
    // Stops everything, including child processes. Call from the thread that called run().
    void stop();

    nlohmann::json facts();      // the /register body and GET /status
    nlohmann::json processes();  // {"rpc": ProcInfo, "head": ProcInfo}

private:
    void setup_routes();
    void control_loop();
    bool do_register();
    int heartbeat();  // HTTP status, 0 = network error
    int default_threads() const;
    std::string coordinator_host_ip();

    AgentOptions opts_;
    httplib::Server server_;
    std::unique_ptr<ManagedProcess> rpc_;
    std::unique_ptr<ManagedProcess> head_;
    std::mutex start_mu_;  // serialises start/stop requests
    xgi::StopFlag stop_flag_;
    std::thread control_thread_;
    std::atomic<bool> stopped_{false};
};

}  // namespace xi
