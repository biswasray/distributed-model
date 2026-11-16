// The coordinator's cluster.yaml: where it lives, what is running, and saving edits from the
// console (architecture/03-coordinator.md §10.1, 05-protocols-and-data.md §5.1).
//
// Saving validates the new text with the normal config parser, writes the file atomically
// (keeping cluster.yaml.bak), and applies the sections that are safe to change while running:
// pools, spares, pinned, llama_cpp and console (except console.enabled). The token, the
// coordinator and router sections, and console.enabled apply on the next XGI start.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "common/config.hpp"

namespace xgi {

// Where xgi finds its config when --config is not given: ./cluster.yaml, then cluster.yaml next
// to the executable. If neither exists, `found` is false and `path` is where a save would go
// (next to the executable).
struct ConfigLocation {
    std::filesystem::path path;
    bool found = false;
};
ConfigLocation locate_config(const std::optional<std::string>& explicit_path, const std::filesystem::path& exe_dir);

// Dotted names of settings that differ between two configs, e.g. "router.port", "pools".
std::vector<std::string> live_changes(const ClusterConfig& running, const ClusterConfig& next);
std::vector<std::string> restart_changes(const ClusterConfig& running, const ClusterConfig& next);
// `running` with the live-applicable sections of `next`.
ClusterConfig merge_live(ClusterConfig running, const ClusterConfig& next);

struct ConfigCheck {
    bool ok = false;
    std::string error;
    ClusterConfig parsed;  // as xgi would run it (QWEN_CLUSTER_TOKEN applied)
    std::string yaml;      // the text that would be written
    std::vector<std::string> warnings, live, restart;
    nlohmann::json to_json() const;
};

class ConfigStore {
public:
    ConfigStore(ClusterConfig running, ConfigLocation where, std::optional<std::string> env_token);

    std::shared_ptr<const ClusterConfig> current() const;
    // GET /console/config body.
    nlohmann::json describe() const;
    // Validate without saving. `yaml` is written as-is; `config` (JSON, cluster.yaml keys) is
    // rendered to YAML, keeping the file's own cluster_token.
    ConfigCheck check_yaml(const std::string& yaml) const;
    ConfigCheck check_json(const nlohmann::json& config) const;
    // Writes a successful check and applies its live sections. On an I/O error, ok becomes false.
    ConfigCheck save(ConfigCheck c);
    // Called (outside the lock) after every save with the new running config.
    void on_change(std::function<void(const ClusterConfig&)> fn);

private:
    std::optional<std::string> read_file() const;
    ConfigCheck diff(ConfigCheck c) const;

    mutable std::mutex mu_;
    std::shared_ptr<const ClusterConfig> running_;
    ConfigLocation where_;
    std::optional<std::string> env_token_;
    std::vector<std::function<void(const ClusterConfig&)>> listeners_;
};

}  // namespace xgi
