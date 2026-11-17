// XGI web console (architecture/03-coordinator.md §8, ADR-009).
//
// Serves the dashboard (embedded HTML/CSS/JS) at /ui/ on the coordinator port, and the
// /console/* API it calls: code generation through the router, and code execution on a node
// through its agent's POST /exec. The page itself is public; every API call needs X-Cluster-Token.
#pragma once

#include <httplib.h>

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "common/config.hpp"

namespace xi {
class Downloads;
}

namespace xgi {

class Coordinator;
class ConfigStore;

class Console {
public:
    // `startup` holds the settings fixed for this process (token, console.enabled);
    // everything else is read from `store`, which a config save can change.
    Console(const ClusterConfig& startup, ConfigStore& store, Coordinator& coordinator,
            std::filesystem::path log_dir = "logs");
    ~Console();
    void register_routes(httplib::Server& server);

    // The node_id the console uses for downloads onto the XGI machine itself (into console.models_dir).
    static constexpr const char* kLocalNode = "@xgi";

    // Paths served without X-Cluster-Token: "/" and the static files under /ui/.
    static bool is_public_path(const std::string& path);

private:
    void on_info(const httplib::Request&, httplib::Response&);
    void on_generate(const httplib::Request&, httplib::Response&);
    void on_exec(const httplib::Request&, httplib::Response&);
    void on_config_get(const httplib::Request&, httplib::Response&);
    void on_config_post(const httplib::Request&, httplib::Response&, bool save);
    // Model downloads, run by the agents (ADR-011).
    void on_hub_files(const httplib::Request&, httplib::Response&);
    void on_downloads_list(const httplib::Request&, httplib::Response&);
    void on_downloads_start(const httplib::Request&, httplib::Response&);
    void on_download_action(const httplib::Request&, httplib::Response&, const std::string& action);
    nlohmann::json local_info();  // {"node_id", "available", "tool", "models_dir", "disk_free", "models"}

    const ClusterConfig& startup_;
    ConfigStore& store_;
    Coordinator& coord_;
    std::unique_ptr<xi::Downloads> local_;  // downloads onto this machine
};

// --- pure helpers (unit-tested)

// Pulls the program out of a model reply: drops <think>…</think>, then returns the body of the
// first fenced code block, or the whole (trimmed) reply if there is none.
std::string extract_code(std::string_view reply);

// The chat messages sent to the router. `node` is the target's node record (may be null);
// `history` is earlier [{role, content}] turns from the UI (may be null).
nlohmann::json build_generate_messages(const std::string& language, const nlohmann::json& node,
                                       const std::string& prompt, const nlohmann::json& history);

// MIME type for a static file name.
std::string mime_type(std::string_view path);

}  // namespace xgi
