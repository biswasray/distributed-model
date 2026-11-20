// XGI web console (architecture/03-coordinator.md §8, ADR-009).
//
// Serves the dashboard (embedded HTML/CSS/JS) at /ui/ on the coordinator port, and the
// /console/* API it calls: code generation through the router, and code execution on a node
// through its agent's POST /exec. The page itself is public; every API call needs X-Cluster-Token.
#pragma once

#include <httplib.h>

#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "common/config.hpp"

namespace xi {
class Downloads;
class ModelServer;
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
    void on_peer_files(const httplib::Request&, httplib::Response&);  // LAN copies (ADR-012)
    // {"node_id", "available", "tool", "models_dir", "disk_free", "models", "model_sizes"}
    nlohmann::json local_info();

    const ClusterConfig& startup_;
    ConfigStore& store_;
    Coordinator& coord_;
    std::unique_ptr<xi::Downloads> local_;  // downloads onto this machine
    std::unique_ptr<xi::ModelServer> served_;  // console.models_dir served to agents for LAN copies
};

// --- LAN copies (ADR-012)

// Model files XGI sends to agents at once.
inline constexpr size_t kXgiMaxUploads = 4;

// Every .gguf file the LAN can copy: {"files": [{"path", "size_bytes", "holders": [node ids, "@xgi"]}]}.
// `nodes` is GET /nodes; only alive nodes that report model_sizes (agents that serve files) count.
// `local_sizes` is the XGI machine's {"path": size} (or null).
nlohmann::json peer_file_union(const nlohmann::json& nodes, const nlohmann::json& local_sizes);

// For source "auto": for each receiver, a node id (or "@xgi") that has every file in `files`, not the
// receiver itself. Sources not in a group come first, then the XGI machine, then nodes in a group;
// receivers are spread round robin. A receiver with no possible source is left out of the result.
std::map<std::string, std::string> plan_peer_sources(const std::vector<std::string>& receivers,
                                                     const nlohmann::json& nodes, const nlohmann::json& local_sizes,
                                                     const std::vector<std::string>& files);

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
