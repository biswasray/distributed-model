// Serving the models folder to other nodes, for LAN copies (02-node-agent.md §12, ADR-012).
//
// GET /models                 {"files": [{"path", "size_bytes"}]}: the .gguf files in the folder
// GET /models/file/<path>     one of them, with HTTP Range support (curl -C - resumes)
//
// Used by XI (its --models-dir, port 8200) and by XGI (console.models_dir, port 8100). The server's
// own pre-routing handler checks X-Cluster-Token. Only complete .gguf files inside the folder are
// served, and only `max_uploads` at once (more get 429 + Retry-After).
#pragma once

#include <httplib.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace xi {

class ModelServer {
public:
    static constexpr int kRetryAfterS = 20;

    ModelServer(std::function<std::filesystem::path()> models_dir, size_t max_uploads);
    void register_routes(httplib::Server& s);
    size_t active_uploads() const { return state_->active.load(); }

private:
    struct State {
        std::atomic<size_t> active{0};
    };
    std::function<std::filesystem::path()> models_dir_;
    size_t max_uploads_;
    std::shared_ptr<State> state_ = std::make_shared<State>();  // outlives a response still being sent
};

// {"files": [{"path", "size_bytes"}]} for the .gguf files under `models_dir`.
nlohmann::json model_files_json(const std::filesystem::path& models_dir);

}  // namespace xi
