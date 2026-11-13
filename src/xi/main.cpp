// xi.exe — XI (Extended Interface), the Qwen Cluster node agent. See architecture/02-node-agent.md.
#include <windows.h>

#include <iostream>

#include "common/cli.hpp"
#include "common/http_util.hpp"
#include "common/log.hpp"
#include "common/util.hpp"
#include "xi/agent.hpp"

namespace {

xi::Agent* g_agent = nullptr;

BOOL WINAPI on_console_event(DWORD) {
    if (g_agent) g_agent->request_shutdown();
    return TRUE;
}

const char* kUsage = R"(xi - XI (Extended Interface): Qwen Cluster node agent

Usage: xi --coordinator http://<control-ip>:8100 [options]

  --coordinator URL        Coordinator base URL (required)
  --token TOKEN            Cluster token (default: env QWEN_CLUSTER_TOKEN)
  --host HOST              Agent API interface (default 0.0.0.0)
  --port PORT              Agent API port (default 8200)
  --name NAME              node_id, unique in the cluster (default: hostname)
  --advertise-ip IP        IP other nodes use to reach this one (default: auto)
  --llama-dir DIR          Folder with rpc-server.exe and llama-server.exe (default .\llama.cpp)
  --models-dir DIR         Folder scanned recursively for *.gguf (default .\models)
  --log-dir DIR            Agent and child process logs (default .\logs)
  --rpc-port PORT          rpc-server port (default 50052)
  --head-port PORT         llama-server port (default 8080)
  --rpc-server-bin PATH    Override rpc-server binary (.py runs with python)
  --llama-server-bin PATH  Override llama-server binary (.py runs with python)
  --heartbeat-s SECONDS    Heartbeat interval (default 5)
  --log-level LEVEL        debug | info | warn | error (default info)
  --version                Print the version
)";

}  // namespace

int main(int argc, char** argv) {
    xgi::CliArgs args(argc, argv);
    if (args.has("--help") || args.has("-h")) {
        std::cout << kUsage;
        return 0;
    }
    if (args.has("--version")) {
        std::cout << "xi " << xgi::kVersion << "\n";
        return 0;
    }
    static const std::set<std::string> known = {
        "--coordinator", "--token",  "--host",      "--port",           "--name",
        "--advertise-ip", "--llama-dir", "--models-dir", "--log-dir",   "--rpc-port",
        "--head-port",   "--rpc-server-bin", "--llama-server-bin", "--heartbeat-s", "--log-level"};
    if (auto bad = args.unknown(known); !bad.empty()) {
        std::cerr << "xi: unknown option " << bad.front() << "\n\n" << kUsage;
        return 2;
    }

    xi::AgentOptions o;
    try {
        o.coordinator = args.get_or("--coordinator", "");
        if (o.coordinator.empty() || !xgi::parse_base_url(o.coordinator)) {
            std::cerr << "xi: --coordinator http://<ip>:<port> is required\n\n" << kUsage;
            return 2;
        }
        o.token = args.get("--token").value_or(xgi::get_env(xgi::kTokenEnv).value_or(""));
        o.host = args.get_or("--host", o.host);
        o.port = args.get_int("--port", o.port);
        o.name = args.get_or("--name", "");
        o.advertise_ip = args.get_or("--advertise-ip", "");
        o.llama_dir = xgi::widen(args.get_or("--llama-dir", ".\\llama.cpp"));
        o.models_dir = xgi::widen(args.get_or("--models-dir", ".\\models"));
        o.log_dir = xgi::widen(args.get_or("--log-dir", ".\\logs"));
        o.rpc_port = args.get_int("--rpc-port", o.rpc_port);
        o.head_port = args.get_int("--head-port", o.head_port);
        if (auto v = args.get("--rpc-server-bin")) o.rpc_server_bin = xgi::widen(*v);
        if (auto v = args.get("--llama-server-bin")) o.llama_server_bin = xgi::widen(*v);
        o.heartbeat_s = args.get_double("--heartbeat-s", o.heartbeat_s);
        if (o.heartbeat_s <= 0) throw std::invalid_argument("--heartbeat-s must be > 0");
    } catch (const std::exception& e) {
        std::cerr << "xi: " << e.what() << "\n";
        return 2;
    }

    xgi::log::init("xi", o.log_dir, xgi::log::parse_level(args.get_or("--log-level", "info")));
    xgi::ensure_winsock();

    xi::Agent agent(o);
    g_agent = &agent;
    SetConsoleCtrlHandler(on_console_event, TRUE);
    bool ok = agent.run();
    agent.stop();
    g_agent = nullptr;
    return ok ? 0 : 1;
}
