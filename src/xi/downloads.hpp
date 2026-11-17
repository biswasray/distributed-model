// Model downloads on a node (02-node-agent.md §11, ADR-011).
//
// XI fetches GGUF files from a Hugging Face model repo or a GitHub release into its models
// folder, with the system curl (Windows 10+, Linux and macOS ship it), so XI needs no TLS library.
// A file downloads to <target>.part and is renamed when complete. Pause stops curl and keeps the
// .part file; resume (or a new download of the same file) continues it with an HTTP Range request.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/hub.hpp"

namespace xi {

namespace fs = std::filesystem;
using json = nlohmann::json;

class ManagedProcess;

// An API error with its HTTP status.
struct DownloadError : std::runtime_error {
    int status;
    std::string type;
    DownloadError(int s, std::string t, const std::string& msg) : std::runtime_error(msg), status(s), type(std::move(t)) {}
};

// POST /downloads body (05 §3.3).
struct DownloadRequest {
    xgi::hub::Source source = xgi::hub::Source::huggingface;
    std::string repo, ref, dest_dir;
    std::vector<std::string> files;
    static constexpr size_t kMaxFiles = 64;
    static DownloadRequest from_json(const json& j);  // throws std::invalid_argument
};

// Where `file` (a repo path) is saved: <models_dir>/<dest_dir>/<file name>. Only the file name
// is kept, so the parts of a split GGUF land next to each other. nullopt if it would escape.
std::optional<fs::path> download_target(const fs::path& models_dir, const std::string& dest_dir, const std::string& file);

// Reads curl's output: the HTTP status printed by -w "%{http_code}" and its "curl: (N) ..." error line.
struct CurlOutcome {
    int http_status = 0;
    std::string error;
};
CurlOutcome parse_curl_output(const std::vector<std::string>& lines);

class Downloads {
public:
    struct Paths {
        fs::path models_dir, log_dir;
    };
    static constexpr size_t kMaxActive = 3;   // jobs downloading at once
    static constexpr size_t kMaxKept = 50;    // finished jobs are dropped beyond this

    // `paths` is read when a job starts (settings can change at run time). `on_file_done` runs
    // after each completed file, so the agent can tell the coordinator about the new model.
    Downloads(std::function<Paths()> paths, std::function<void()> on_file_done);
    ~Downloads();
    Downloads(const Downloads&) = delete;
    Downloads& operator=(const Downloads&) = delete;

    bool available() const { return !curl_.empty(); }
    json tool_json() const;  // {"available", "tool"}: reported in the node facts

    json list();                                    // {"downloads": [...]} newest first
    json start(const DownloadRequest& r);           // the new job; throws DownloadError
    json pause(const std::string& id);
    json resume(const std::string& id);
    json cancel(const std::string& id);
    void remove(const std::string& id);             // finished jobs only
    // GET /hub/files: the files in a repo or release (GGUF only unless `all`). Throws DownloadError.
    json hub_files(xgi::hub::Source s, const std::string& repo, const std::string& ref, bool all);

    // Pauses every running job (their .part files stay) and waits for the threads.
    void shutdown();

private:
    struct File {
        std::string path;          // in the repo
        fs::path target;           // absolute
        std::string target_rel;    // relative to the models folder, '/'-separated
        std::optional<std::uint64_t> size;
        std::uint64_t done = 0;
        std::string state = "pending";  // pending | downloading | done | present
    };
    struct Job {
        std::string id, source, repo, ref, dest_dir;
        xgi::hub::Source src = xgi::hub::Source::huggingface;
        std::vector<File> files;
        std::string state = "starting";  // starting | downloading | pausing | paused | completed | failed | canceled
        std::string error;
        std::string stop;                // "" | pause | cancel | shutdown
        bool listed = false;             // sizes and ref resolved from the hub listing
        bool running = false;            // the job thread is alive
        double created_at = 0, updated_at = 0, finished_at = 0;
        double speed_bps = 0;
        size_t current = 0;
        std::shared_ptr<ManagedProcess> proc;
        std::thread thread;
    };
    json job_json(const Job& j) const;  // under mu_
    std::shared_ptr<Job> find(const std::string& id);
    void launch(const std::shared_ptr<Job>& j);  // under mu_
    void run(std::shared_ptr<Job> j);
    bool download_file(const std::shared_ptr<Job>& j, size_t index, const Paths& paths);  // false = stopped or failed
    void finish(const std::shared_ptr<Job>& j, const std::string& state, const std::string& error);
    std::vector<std::string> auth_args(xgi::hub::Source s, const fs::path& dir, std::string& header_file) const;
    bool stop_requested(const std::shared_ptr<Job>& j);
    void prune();  // under mu_

    std::function<Paths()> paths_;
    std::function<void()> on_file_done_;
    xgi::hub::Bases bases_;
    fs::path curl_;
    std::string curl_version_;

    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    bool shutting_down_ = false;
};

}  // namespace xi
