// Language runtimes on a node, and running console code with them (02-node-agent.md §8, ADR-009).
//
// XI detects which interpreters/compilers are on PATH at startup and reports them as `runtimes`.
// When the agent runs with --allow-exec, POST /exec writes the code to a temporary job folder and
// runs it with one of the detected runtimes, with a timeout and captured output.
#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace xi {

namespace fs = std::filesystem;
using json = nlohmann::json;

// One language XI knows how to run. `candidates` are executable names tried in order on PATH.
struct RuntimeSpec {
    std::string id;        // "python", "javascript", "powershell", ... (the API's `language`)
    std::string label;     // "Python", "JavaScript (Node.js)", ...
    std::string ext;       // source file extension, e.g. ".py"
    std::vector<std::string> candidates;
    std::vector<std::string> version_args;
    bool compiled = false;  // compile to an executable first, then run it
};

// A runtime found on this node.
struct Runtime {
    std::string id, label, version;
    fs::path exe;
    json to_json() const;  // {"language", "label", "version", "path"}
};

// The specs that apply to this OS (e.g. `cmd` only on Windows, `sh` only on POSIX).
const std::vector<RuntimeSpec>& runtime_specs();
const RuntimeSpec* find_spec(const std::string& id);

// Searches PATH for `name` (adds .exe/.com on Windows). Skips the Windows WSL launcher
// (System32\bash.exe), which can't run a Windows path.
std::optional<fs::path> find_on_path(const std::string& name);

// The first non-empty line of a version probe's output, trimmed and capped at 120 characters.
std::string first_line(const std::vector<std::string>& lines);

// Detects every runtime in parallel (each probe has `probe_timeout_s`). Sorted by id.
std::vector<Runtime> detect_runtimes(const fs::path& log_file, double probe_timeout_s = 5);

struct ExecRequest {
    std::string language;
    std::string code;
    double timeout_s = 60;
    static constexpr size_t kMaxCodeBytes = 256 * 1024;
    // Throws std::invalid_argument. `max_timeout_s` caps timeout_s.
    static ExecRequest from_json(const json& j, double max_timeout_s);
};

// Command lines for one job: optional compile step, then the run step.
struct ExecPlan {
    std::optional<std::vector<std::string>> compile;
    std::vector<std::string> run;
};
// `source` is the job's code file; `binary` is where a compiled program goes.
ExecPlan build_exec_plan(const RuntimeSpec& spec, const fs::path& exe, const fs::path& source, const fs::path& binary);

// Removes the "[xi] started/stopped pid" marker lines ManagedProcess adds, and caps each line.
std::vector<std::string> clean_output(std::vector<std::string> lines, size_t max_line_chars = 4000);

// Runs one job to completion. `work_dir` is the program's working directory; the code itself is
// written to work_dir/.jobs/<id>/ and removed afterwards. Returns the /exec response body.
json run_job(const Runtime& rt, const ExecRequest& req, const fs::path& work_dir, const fs::path& log_file);

}  // namespace xi
