#include "xi/commands.hpp"

#include <algorithm>
#include <stdexcept>

#include "common/config.hpp"
#include "common/util.hpp"

namespace xi {
namespace {

std::optional<int> opt_int(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return std::nullopt;
    if (!j[key].is_number_integer()) throw std::invalid_argument(std::string(key) + " must be an integer or null");
    return j[key].get<int>();
}

std::vector<std::string> str_list(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return {};
    if (!j[key].is_array()) throw std::invalid_argument(std::string(key) + " must be a list of strings");
    std::vector<std::string> out;
    for (const auto& v : j[key]) {
        if (!v.is_string()) throw std::invalid_argument(std::string(key) + " must be a list of strings");
        out.push_back(v.get<std::string>());
    }
    return out;
}

}  // namespace

RpcStartRequest RpcStartRequest::from_json(const json& j) {
    if (!j.is_object()) throw std::invalid_argument("body must be a JSON object");
    RpcStartRequest r;
    r.threads = opt_int(j, "threads");
    if (j.contains("cache") && !j["cache"].is_null()) {
        if (!j["cache"].is_boolean()) throw std::invalid_argument("cache must be a boolean");
        r.cache = j["cache"].get<bool>();
    }
    if (r.threads && *r.threads < 1) throw std::invalid_argument("threads must be >= 1");
    return r;
}

HeadStartRequest HeadStartRequest::from_json(const json& j) {
    if (!j.is_object()) throw std::invalid_argument("body must be a JSON object");
    HeadStartRequest r;
    if (!j.contains("model_file") || !j["model_file"].is_string() || j["model_file"].get<std::string>().empty())
        throw std::invalid_argument("model_file is required");
    r.model_file = j["model_file"].get<std::string>();
    r.rpc_endpoints = str_list(j, "rpc_endpoints");
    r.context = opt_int(j, "context").value_or(r.context);
    r.parallel = opt_int(j, "parallel").value_or(r.parallel);
    r.threads = opt_int(j, "threads");
    r.ngl = opt_int(j, "ngl").value_or(r.ngl);
    if (j.contains("tensor_split") && !j["tensor_split"].is_null()) {
        if (!j["tensor_split"].is_array()) throw std::invalid_argument("tensor_split must be a list of numbers");
        std::vector<double> v;
        for (const auto& x : j["tensor_split"]) {
            if (!x.is_number()) throw std::invalid_argument("tensor_split must be a list of numbers");
            v.push_back(x.get<double>());
        }
        r.tensor_split = std::move(v);
    }
    r.extra_args = str_list(j, "extra_args");
    if (r.context < 1 || r.parallel < 1) throw std::invalid_argument("context and parallel must be >= 1");
    if (r.threads && *r.threads < 1) throw std::invalid_argument("threads must be >= 1");
    return r;
}

std::vector<std::string> build_rpc_args(int rpc_port, int threads, bool cache) {
    std::vector<std::string> a{"-H", "0.0.0.0", "-p", std::to_string(rpc_port), "-t", std::to_string(threads)};
    if (cache) a.push_back("-c");
    return a;
}

std::vector<std::string> build_head_args(const fs::path& model_path, int head_port, const HeadStartRequest& r,
                                         int threads) {
    std::vector<std::string> a{
        "-m", xgi::narrow(model_path.wstring()),
        "--host", "0.0.0.0",
        "--port", std::to_string(head_port),
        // llama-server divides -c evenly between slots; keep per-request context = pool context.
        "-c", std::to_string(static_cast<long long>(r.context) * r.parallel),
        "-np", std::to_string(r.parallel),
        "-t", std::to_string(threads),
        "-ngl", std::to_string(r.ngl),
    };
    if (!r.rpc_endpoints.empty()) {
        a.push_back("--rpc");
        a.push_back(xgi::join(r.rpc_endpoints, ","));
    }
    if (r.tensor_split && !r.tensor_split->empty()) {
        std::vector<std::string> parts;
        for (double d : *r.tensor_split) parts.push_back(xgi::format_number(d));
        a.push_back("--tensor-split");
        a.push_back(xgi::join(parts, ","));
    }
    a.insert(a.end(), r.extra_args.begin(), r.extra_args.end());
    return a;
}

std::optional<fs::path> resolve_model_path(const fs::path& models_dir, const std::string& model_file) {
    if (!xgi::is_valid_model_file(model_file)) return std::nullopt;
    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::absolute(models_dir, ec), ec);
    if (ec) return std::nullopt;
    fs::path candidate = fs::weakly_canonical(root / xgi::widen(xgi::normalize_model_path(model_file)), ec);
    if (ec) return std::nullopt;
    // After resolving symlinks the file must still be inside the models dir.
    fs::path rel = candidate.lexically_relative(root);
    if (rel.empty() || rel.is_absolute() || *rel.begin() == L"..") return std::nullopt;
    if (!fs::is_regular_file(candidate, ec)) return std::nullopt;
    return candidate;
}

std::vector<std::string> scan_models(const fs::path& models_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(models_dir, ec)) return out;
    fs::recursive_directory_iterator it(models_dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        if (!it->is_regular_file(ec)) continue;
        if (!xgi::ends_with_ci(xgi::narrow(p.extension().wstring()), ".gguf")) continue;
        out.push_back(xgi::normalize_model_path(xgi::narrow(p.lexically_relative(models_dir).generic_wstring())));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> program_argv(const fs::path& bin, const std::vector<std::string>& args) {
    std::vector<std::string> argv;
    if (xgi::ends_with_ci(xgi::narrow(bin.extension().wstring()), ".py")) argv.push_back("python");
    argv.push_back(xgi::narrow(bin.wstring()));
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
}

std::wstring quote_windows_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(*it);
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring build_command_line(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd.push_back(L' ');
        cmd += quote_windows_arg(xgi::widen(argv[i]));
    }
    return cmd;
}

}  // namespace xi
