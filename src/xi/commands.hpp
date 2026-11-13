// Pure helpers for XI: llama.cpp command lines (02-node-agent.md §4.1) and model paths (ADR-005).
#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace xi {

namespace fs = std::filesystem;
using json = nlohmann::json;

struct RpcStartRequest {
    std::optional<int> threads;
    bool cache = true;
    static RpcStartRequest from_json(const json& j);  // throws std::invalid_argument
};

struct HeadStartRequest {
    std::string model_file;
    std::vector<std::string> rpc_endpoints;
    int context = 4096;
    int parallel = 1;
    std::optional<int> threads;
    int ngl = 99;
    std::optional<std::vector<double>> tensor_split;
    std::vector<std::string> extra_args;
    static HeadStartRequest from_json(const json& j);  // throws std::invalid_argument
};

// rpc-server -H 0.0.0.0 -p <rpc_port> -t <threads> [-c]
std::vector<std::string> build_rpc_args(int rpc_port, int threads, bool cache);

// llama-server -m <model> --host 0.0.0.0 --port <p> -c <context*parallel> -np <parallel> -t <threads>
//              -ngl <ngl> [--rpc a,b] [--tensor-split x,y] [extra_args...]
std::vector<std::string> build_head_args(const fs::path& model_path, int head_port, const HeadStartRequest& r,
                                         int threads);

// Resolves `model_file` inside `models_dir`. Returns nullopt if it is absolute, escapes the folder
// (.. or symlink tricks), or is not an existing regular file.
std::optional<fs::path> resolve_model_path(const fs::path& models_dir, const std::string& model_file);

// All *.gguf files under models_dir (recursive), relative, '/'-separated, sorted.
std::vector<std::string> scan_models(const fs::path& models_dir);

// Program + args for a binary. A ".py" binary runs as `python <script> args...` (python3 on POSIX).
std::vector<std::string> program_argv(const fs::path& bin, const std::vector<std::string>& args);

// Quotes one UTF-8 argument for a Windows command line (CommandLineToArgvW rules). The rules only
// involve ASCII characters, so this works byte-wise on UTF-8. Used by the Windows process backend.
std::string quote_windows_arg(const std::string& arg);
std::string build_command_line(const std::vector<std::string>& argv);

}  // namespace xi
