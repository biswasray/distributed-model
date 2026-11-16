// XGI web console (architecture/03-coordinator.md §8, ADR-009).
//
// Serves the dashboard (embedded HTML/CSS/JS) at /ui/ on the coordinator port, and the
// /console/* API it calls: code generation through the router, and code execution on a node
// through its agent's POST /exec. The page itself is public; every API call needs X-Cluster-Token.
#pragma once

#include <httplib.h>

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "common/config.hpp"

namespace xgi {

class Coordinator;

class Console {
public:
    Console(const ClusterConfig& cfg, Coordinator& coordinator);
    void register_routes(httplib::Server& server);

    // Paths served without X-Cluster-Token: "/" and the static files under /ui/.
    static bool is_public_path(const std::string& path);

private:
    void on_info(const httplib::Request&, httplib::Response&);
    void on_generate(const httplib::Request&, httplib::Response&);
    void on_exec(const httplib::Request&, httplib::Response&);

    const ClusterConfig& cfg_;
    Coordinator& coord_;
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
