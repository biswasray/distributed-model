// xi — XI (Extended Interface), the Qwen Cluster node agent. See architecture/02-node-agent.md.
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#include "common/cli.hpp"
#include "common/http_util.hpp"
#include "common/log.hpp"
#include "common/net.hpp"
#include "common/platform.hpp"
#include "common/shutdown.hpp"
#include "common/sysinfo.hpp"
#include "common/util.hpp"
#include "xi/agent.hpp"

namespace {

const char* kUsage = R"(xi - XI (Extended Interface): Qwen Cluster node agent

Usage: xi [--coordinator http://<control-ip>:8100] [options]

Settings come from built-in defaults, then xi.yaml, then these flags (flags win). Every
setting except the token can also be changed on the agent's web page, http://<node>:8200/ui/,
which saves it to xi.yaml.

  --config PATH            Settings file (default: ./xi.yaml, else xi.yaml next to xi; created
                           by the first save from the web page)
  --coordinator URL        Coordinator base URL (without it, the agent waits to be configured)
  --token TOKEN            Cluster token (default: env QWEN_CLUSTER_TOKEN; never stored in xi.yaml)
  --host HOST              Agent API interface (default 0.0.0.0)
  --port PORT              Agent API and web page port (default 8200)
  --name NAME              node_id, unique in the cluster (default: hostname)
  --advertise-ip IP        IP other nodes use to reach this one (default: auto)
  --llama-dir DIR          Folder with rpc-server and llama-server (default ./llama.cpp)
  --models-dir DIR         Folder scanned recursively for *.gguf (default ./models)
  --log-dir DIR            Agent and child process logs (default ./logs)
  --rpc-port PORT          rpc-server port (default 50052)
  --head-port PORT         llama-server port (default 8080)
  --rpc-server-bin PATH    Override rpc-server binary (.py runs with python)
  --llama-server-bin PATH  Override llama-server binary (.py runs with python)
  --heartbeat-s SECONDS    Heartbeat interval (default 5)
  --allow-exec             Let the XGI console run code on this node (POST /exec). Off by default.
  --exec-dir DIR           Working directory for console programs (default ./exec)
  --exec-max-timeout-s S   Longest run a console job may ask for (default 300)
  --log-level LEVEL        debug | info | warn | error (default info)
  --version                Print the version
)";

// Settings given as flags, as JSON with xi.yaml keys.
xgi::json flag_settings(const xgi::CliArgs& args) {
    static const std::set<std::string> ints{"port", "rpc_port", "head_port"};
    static const std::set<std::string> doubles{"heartbeat_s", "exec_max_timeout_s"};
    xgi::json j = xgi::json::object();
    for (const auto& key : xi::settings_keys()) {
        const std::string flag = xi::flag_for(key);
        if (key == "allow_exec") {
            if (args.has(flag)) j[key] = true;
        } else if (args.has(flag)) {
            if (ints.contains(key)) j[key] = args.get_int(flag, 0);
            else if (doubles.contains(key)) j[key] = args.get_double(flag, 0);
            else j[key] = args.get_or(flag, "");
        }
    }
    return j;
}

}  // namespace

int main(int argc, char** argv) {
    xgi::CliArgs args(argc, argv, {"--help", "-h", "--version", "--allow-exec"});
    if (args.has("--help") || args.has("-h")) {
        std::cout << kUsage;
        return 0;
    }
    if (args.has("--version")) {
        std::cout << "xi " << xgi::kVersion << " (" << xgi::kPlatformName << ")\n";
        return 0;
    }
    std::set<std::string> known = {"--config", "--token", "--log-level"};
    for (const auto& k : xi::settings_keys()) known.insert(xi::flag_for(k));
    if (auto bad = args.unknown(known); !bad.empty()) {
        std::cerr << "xi: unknown option " << bad.front() << "\n\n" << kUsage;
        return 2;
    }

    xi::AgentBoot boot;
    try {
        const auto exe = xgi::sysinfo::executable_path();
        boot.file = xi::locate_settings(args.get("--config"), exe.empty() ? exe : exe.parent_path());
        if (boot.file.found) {
            std::ifstream f(boot.file.path, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            boot.file_settings = xi::parse_settings_yaml(ss.str());
        }
        xgi::json flags = flag_settings(args);
        for (auto it = flags.begin(); it != flags.end(); ++it) boot.cli_keys.insert(it.key());
        boot.settings = xi::apply_json(boot.file_settings, flags);
        if (auto t = args.get("--token")) {
            boot.token = *t;
            boot.token_source = "flag";
        } else if (auto e = xgi::get_env(xgi::kTokenEnv); e && !e->empty()) {
            boot.token = *e;
            boot.token_source = "env";
        }
    } catch (const std::exception& e) {
        std::cerr << "xi: " << e.what() << "\n";
        return 2;
    }

    xgi::log::init("xi", xgi::path_from_utf8(boot.settings.log_dir), xgi::log::parse_level(args.get_or("--log-level", "info")));
    xgi::ensure_sockets();
    if (boot.settings.coordinator.empty())
        xgi::log::warn("no coordinator set: open http://127.0.0.1:{}/ui/ on this node (or pass --coordinator)",
                       boot.settings.port);

    xi::Agent agent(boot);
    xgi::install_shutdown_handler([&agent] { agent.request_shutdown(); });
    bool ok = agent.run();
    agent.stop();
    return ok ? 0 : 1;
}
