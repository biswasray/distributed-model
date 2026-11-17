// cluster.yaml schema and validation (architecture/05-protocols-and-data.md §5).
#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace xgi {

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct PoolConfig {
    std::string name;
    std::string model_file;  // relative to each head's --models-dir, '/' separators
    int nodes_per_group = 1;
    int groups = 0;
    int context = 4096;
    std::optional<int> parallel;  // slots per group; default router.slots_per_group
    std::optional<std::vector<double>> tensor_split;
    std::vector<std::string> extra_args;
    bool operator==(const PoolConfig&) const = default;
};

struct PinnedEntry {
    std::string host;
    std::string role = "head";
    std::string pool;
    bool operator==(const PinnedEntry&) const = default;
};

struct CoordinatorConfig {
    std::string host = "0.0.0.0";
    int port = 8100;
    double heartbeat_interval_s = 5;
    double node_timeout_s = 30;
    double group_start_timeout_s = 7200;
    bool operator==(const CoordinatorConfig&) const = default;
};

struct RouterConfig {
    std::string host = "0.0.0.0";
    int port = 8000;
    std::string coordinator_url = "http://127.0.0.1:8100";
    int max_queue = 100;
    double queue_timeout_s = 600;
    double request_timeout_s = 1800;
    int slots_per_group = 1;
    std::vector<std::string> api_keys;
    bool operator==(const RouterConfig&) const = default;
};

struct LlamaCppConfig {
    bool rpc_cache = true;
    std::vector<std::string> extra_head_args;
    bool operator==(const LlamaCppConfig&) const = default;
};

// Web console on the coordinator (architecture/03-coordinator.md §8, ADR-009).
struct ConsoleConfig {
    bool enabled = true;          // serve the dashboard at /ui/
    bool allow_exec = false;      // allow POST /console/exec (agents must also run with --allow-exec)
    std::string router_url = "http://127.0.0.1:8000";  // where code generation is sent
    std::string model;            // pool used for code generation; empty = first pool
    int max_tokens = 1024;
    double generate_timeout_s = 600;
    double exec_timeout_s = 60;   // default per-run timeout offered by the UI
    std::string models_dir = "models";  // where XGI's own model downloads go (ADR-011); relative = working dir
    bool operator==(const ConsoleConfig&) const = default;
};

struct ClusterConfig {
    std::string cluster_token;  // after applying QWEN_CLUSTER_TOKEN precedence
    CoordinatorConfig coordinator;
    RouterConfig router;
    LlamaCppConfig llama_cpp;
    ConsoleConfig console;
    std::vector<PoolConfig> pools;
    int spares = 0;
    std::vector<PinnedEntry> pinned;
    std::vector<std::string> warnings;  // non-fatal issues found while loading

    const PoolConfig* find_pool(const std::string& name) const;
    int slots_for(const PoolConfig& pool) const { return pool.parallel.value_or(router.slots_per_group); }
    int total_nodes_requested() const;
    // The pool the console generates code with: console.model, else the first pool ("" if none).
    std::string console_model() const;
};

// Parses YAML text. Throws ConfigError on any validation rule marked "Startup fails".
// `env_token` (normally QWEN_CLUSTER_TOKEN) overrides cluster_token when set.
ClusterConfig parse_config(const std::string& yaml_text, const std::optional<std::string>& env_token);
ClusterConfig load_config(const std::filesystem::path& path);

// Built-in defaults, used when there is no cluster.yaml: no pools, token from `env_token` if set.
ClusterConfig default_config(const std::optional<std::string>& env_token);

// cluster.yaml text for a config (block style, no comments). parse_config(to_yaml(c)) == c.
// The cluster_token written is c.cluster_token, so pass the file's own value, not the env one.
std::string to_yaml(const ClusterConfig& c);

// The config as JSON with the same keys as cluster.yaml. cluster_token is left out.
nlohmann::json to_json(const ClusterConfig& c);

// "a\\b.gguf" -> "a/b.gguf"; strips a leading "./".
std::string normalize_model_path(std::string p);
// True if `p` is relative, has no ".." component and ends in .gguf.
bool is_valid_model_file(const std::string& p);

}  // namespace xgi
