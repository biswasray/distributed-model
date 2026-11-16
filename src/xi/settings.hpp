// XI settings: the values behind the command-line flags, as stored in xi.yaml and edited on the
// agent's web page (architecture/02-node-agent.md §10, ADR-010).
//
// Precedence at startup: built-in defaults < xi.yaml < command-line flags. The cluster token is
// never stored in xi.yaml: it comes from --token or QWEN_CLUSTER_TOKEN, and the page can change it
// for the running process only.
#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace xi {

struct AgentSettings {
    std::string coordinator;  // base URL; empty = not configured yet (the agent waits)
    std::string name;         // node_id; empty = hostname
    std::string advertise_ip; // empty = the route to the coordinator
    std::string host = "0.0.0.0";
    int port = 8200;
    std::string llama_dir = "llama.cpp";
    std::string models_dir = "models";
    std::string log_dir = "logs";
    int rpc_port = 50052;
    int head_port = 8080;
    std::string rpc_server_bin;    // empty = <llama_dir>/rpc-server[.exe]
    std::string llama_server_bin;  // empty = <llama_dir>/llama-server[.exe]
    double heartbeat_s = 5;
    bool allow_exec = false;
    std::string exec_dir = "exec";
    double exec_max_timeout_s = 300;
    bool operator==(const AgentSettings&) const = default;
};

// Settings that need an XI restart: the API socket, the log folder, and the llama.cpp ports
// (running processes and the coordinator's view of them use the old ones).
const std::set<std::string>& restart_keys();
// Every settings key, in display order. Each maps to the flag "--" + key with '_' -> '-'.
const std::vector<std::string>& settings_keys();
std::string flag_for(const std::string& key);

nlohmann::json to_json(const AgentSettings& s);
// Applies the keys present in `j` onto `base`, then validates. Throws std::invalid_argument.
AgentSettings apply_json(AgentSettings base, const nlohmann::json& j);
void validate(const AgentSettings& s);  // throws std::invalid_argument

// xi.yaml. parse throws std::invalid_argument (unknown keys are errors, so typos are caught).
AgentSettings parse_settings_yaml(const std::string& text);
std::string to_yaml(const AgentSettings& s);

// Dotted-free key names that differ.
std::vector<std::string> changed_keys(const AgentSettings& a, const AgentSettings& b);
// `running` with every key of `next` except the restart keys.
AgentSettings merge_live(AgentSettings running, const AgentSettings& next);

// True for 127.0.0.0/8, ::1 and ::ffff:127.x addresses.
bool is_loopback(const std::string& addr);

// Where xi finds xi.yaml without --config: ./xi.yaml, then xi.yaml next to the executable.
struct SettingsLocation {
    std::filesystem::path path;
    bool found = false;
};
SettingsLocation locate_settings(const std::optional<std::string>& explicit_path, const std::filesystem::path& exe_dir);

}  // namespace xi
