// cluster.yaml schema and validation (architecture/05-protocols-and-data.md §5).
#pragma once

#include <filesystem>
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
};

struct PinnedEntry {
    std::string host;
    std::string role = "head";
    std::string pool;
};

struct CoordinatorConfig {
    std::string host = "0.0.0.0";
    int port = 8100;
    double heartbeat_interval_s = 5;
    double node_timeout_s = 30;
    double group_start_timeout_s = 7200;
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
};

struct LlamaCppConfig {
    bool rpc_cache = true;
    std::vector<std::string> extra_head_args;
};

struct ClusterConfig {
    std::string cluster_token;  // after applying QWEN_CLUSTER_TOKEN precedence
    CoordinatorConfig coordinator;
    RouterConfig router;
    LlamaCppConfig llama_cpp;
    std::vector<PoolConfig> pools;
    int spares = 0;
    std::vector<PinnedEntry> pinned;
    std::vector<std::string> warnings;  // non-fatal issues found while loading

    const PoolConfig* find_pool(const std::string& name) const;
    int slots_for(const PoolConfig& pool) const { return pool.parallel.value_or(router.slots_per_group); }
    int total_nodes_requested() const;
};

// Parses YAML text. Throws ConfigError on any validation rule marked "Startup fails".
// `env_token` (normally QWEN_CLUSTER_TOKEN) overrides cluster_token when set.
ClusterConfig parse_config(const std::string& yaml_text, const std::optional<std::string>& env_token);
ClusterConfig load_config(const std::filesystem::path& path);

// "a\\b.gguf" -> "a/b.gguf"; strips a leading "./".
std::string normalize_model_path(std::string p);
// True if `p` is relative, has no ".." component and ends in .gguf.
bool is_valid_model_file(const std::string& p);

}  // namespace xgi
