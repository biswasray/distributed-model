#include "xi/runtimes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <future>
#include <random>
#include <stdexcept>
#include <thread>

#include "common/log.hpp"
#include "common/platform.hpp"
#include "common/util.hpp"
#include "xi/process.hpp"

namespace xi {

namespace {

constexpr bool kWindows = xgi::kPlatformName == "windows";

std::vector<RuntimeSpec> make_specs() {
    std::vector<RuntimeSpec> s;
    const std::vector<std::string> v{"--version"};
    const std::vector<std::string> ps_version{"-NoProfile", "-NonInteractive", "-Command",
                                              "$PSVersionTable.PSVersion.ToString()"};
    s.push_back({"python", "Python", ".py",
                 kWindows ? std::vector<std::string>{"python", "py", "python3"}
                          : std::vector<std::string>{"python3", "python"},
                 v});
    s.push_back({"javascript", "JavaScript (Node.js)", ".js", {"node"}, v});
    if (kWindows) s.push_back({"powershell", "Windows PowerShell", ".ps1", {"powershell"}, ps_version});
    s.push_back({"pwsh", "PowerShell 7", ".ps1", {"pwsh"}, ps_version});
    s.push_back({"bash", "Bash", ".sh", {"bash"}, v});
    if (!kWindows) s.push_back({"sh", "POSIX sh", ".sh", {"sh"}, {"-c", "echo POSIX sh"}});
    if (kWindows) s.push_back({"cmd", "Batch (cmd.exe)", ".cmd", {"cmd"}, {"/d", "/c", "ver"}});
    s.push_back({"ruby", "Ruby", ".rb", {"ruby"}, v});
    s.push_back({"perl", "Perl", ".pl", {"perl"}, {"-v"}});
    s.push_back({"php", "PHP", ".php", {"php"}, {"-v"}});
    s.push_back({"lua", "Lua", ".lua", {"lua", "lua5.4", "lua54"}, {"-v"}});
    s.push_back({"go", "Go", ".go", {"go"}, {"version"}});
    s.push_back({"java", "Java", ".java", {"java"}, {"-version"}});
    s.push_back({"c", "C", ".c", {"gcc", "clang", "cc"}, v, true});
    s.push_back({"cpp", "C++", ".cpp", {"g++", "clang++", "c++"}, v, true});
    s.push_back({"rust", "Rust", ".rs", {"rustc"}, v, true});
    return s;
}

bool is_executable(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    if (kWindows) return true;
    auto perms = fs::status(p, ec).permissions();
    return (perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
}

struct RunResult {
    std::optional<long long> exit_code;
    bool timed_out = false;
    std::vector<std::string> lines;
    bool truncated = false;
};

// Runs argv until it exits or `deadline` (monotonic) passes, then collects its output.
RunResult run_to_completion(const std::string& role, const fs::path& log_file, const std::vector<std::string>& argv,
                            const fs::path& cwd, double deadline) {
    ManagedProcess p(role, log_file);
    p.start(argv, cwd);
    RunResult r;
    while (true) {
        ProcInfo i = p.info();
        if (i.state != "running") {
            r.exit_code = i.exit_code;
            break;
        }
        if (xgi::mono_now() >= deadline) {
            r.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    p.stop(0);  // kills it (and anything it started) if still running; joins the output reader
    auto raw = p.tail(ManagedProcess::kTailLines);
    r.truncated = raw.size() >= ManagedProcess::kTailLines;
    r.lines = clean_output(std::move(raw));
    return r;
}

std::string random_hex(int bytes) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    std::string out;
    for (int i = 0; i < bytes; ++i) out += std::format("{:02x}", (rd() ^ counter.fetch_add(1)) & 0xff);
    return out;
}

}  // namespace

json Runtime::to_json() const {
    return json{{"language", id}, {"label", label}, {"version", version}, {"path", xgi::path_to_utf8(exe)}};
}

const std::vector<RuntimeSpec>& runtime_specs() {
    static const std::vector<RuntimeSpec> specs = make_specs();
    return specs;
}

const RuntimeSpec* find_spec(const std::string& id) {
    for (const auto& s : runtime_specs())
        if (s.id == id) return &s;
    return nullptr;
}

std::optional<fs::path> find_on_path(const std::string& name) {
    auto path = xgi::get_env("PATH");
    if (!path) return std::nullopt;
    std::vector<std::string> exts{""};
    if (kWindows) exts = {".exe", ".com"};
    for (const auto& dir : xgi::split(*path, kWindows ? ';' : ':')) {
        if (dir.empty()) continue;
        for (const auto& ext : exts) {
            fs::path p = xgi::path_from_utf8(dir) / xgi::path_from_utf8(name + ext);
            if (!is_executable(p)) continue;
            // System32\bash.exe is the WSL launcher: it can't run a Windows path.
            if (kWindows && name == "bash" && xgi::to_lower(xgi::path_to_utf8(p)).find("system32") != std::string::npos)
                continue;
            return p;
        }
    }
    return std::nullopt;
}

std::string first_line(const std::vector<std::string>& lines) {
    for (auto l : lines) {
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t' || l.back() == '\r')) l.pop_back();
        size_t start = l.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        l = l.substr(start);
        if (l.size() > 120) l = l.substr(0, 120);
        return l;
    }
    return {};
}

std::vector<Runtime> detect_runtimes(const fs::path& log_file, double probe_timeout_s) {
    std::vector<std::future<std::optional<Runtime>>> fs;
    for (const auto& spec : runtime_specs()) {
        fs.push_back(std::async(std::launch::async, [&spec, &log_file, probe_timeout_s]() -> std::optional<Runtime> {
            for (const auto& name : spec.candidates) {
                auto exe = find_on_path(name);
                if (!exe) continue;
                std::vector<std::string> argv{xgi::path_to_utf8(*exe)};
                argv.insert(argv.end(), spec.version_args.begin(), spec.version_args.end());
                try {
                    auto r = run_to_completion("probe-" + spec.id, log_file, argv, {}, xgi::mono_now() + probe_timeout_s);
                    // A non-zero exit is e.g. the Microsoft Store "python" stub.
                    if (r.timed_out || !r.exit_code || *r.exit_code != 0) continue;
                    return Runtime{spec.id, spec.label, first_line(r.lines), *exe};
                } catch (const std::exception& e) {
                    xgi::log::debug("runtime probe {} failed: {}", name, e.what());
                }
            }
            return std::nullopt;
        }));
    }
    std::vector<Runtime> out;
    for (auto& f : fs)
        if (auto r = f.get()) out.push_back(std::move(*r));
    std::sort(out.begin(), out.end(), [](const Runtime& a, const Runtime& b) { return a.id < b.id; });
    return out;
}

ExecRequest ExecRequest::from_json(const json& j, double max_timeout_s) {
    if (!j.is_object()) throw std::invalid_argument("body must be a JSON object");
    ExecRequest r;
    if (!j.contains("language") || !j["language"].is_string() || j["language"].get<std::string>().empty())
        throw std::invalid_argument("language is required");
    r.language = j["language"].get<std::string>();
    if (!j.contains("code") || !j["code"].is_string()) throw std::invalid_argument("code is required");
    r.code = j["code"].get<std::string>();
    if (r.code.empty()) throw std::invalid_argument("code must not be empty");
    if (r.code.size() > kMaxCodeBytes)
        throw std::invalid_argument(std::format("code is larger than {} bytes", kMaxCodeBytes));
    if (j.contains("timeout_s") && !j["timeout_s"].is_null()) {
        if (!j["timeout_s"].is_number()) throw std::invalid_argument("timeout_s must be a number");
        r.timeout_s = j["timeout_s"].get<double>();
        if (r.timeout_s <= 0) throw std::invalid_argument("timeout_s must be > 0");
    }
    r.timeout_s = std::min(r.timeout_s, max_timeout_s);
    return r;
}

ExecPlan build_exec_plan(const RuntimeSpec& spec, const fs::path& exe, const fs::path& source, const fs::path& binary) {
    const std::string e = xgi::path_to_utf8(exe);
    const std::string src = xgi::path_to_utf8(source);
    ExecPlan p;
    if (spec.compiled) {
        const std::string bin = xgi::path_to_utf8(binary);
        if (spec.id == "rust") p.compile = std::vector<std::string>{e, "-O", src, "-o", bin};
        else if (spec.id == "cpp") p.compile = std::vector<std::string>{e, "-std=c++17", "-O2", src, "-o", bin};
        else p.compile = std::vector<std::string>{e, "-O2", src, "-o", bin};
        p.run = {bin};
        return p;
    }
    if (spec.id == "python") p.run = {e, "-X", "utf8", src};  // UTF-8 stdout even on a Windows pipe
    else if (spec.id == "powershell" || spec.id == "pwsh")
        p.run = {e, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", src};
    else if (spec.id == "bash" || spec.id == "sh") p.run = {e, xgi::generic_utf8(source)};  // Git Bash wants '/'
    else if (spec.id == "cmd") p.run = {e, "/d", "/c", src};
    else if (spec.id == "go") p.run = {e, "run", src};
    else p.run = {e, src};  // node, ruby, perl, php, lua, java (single-file source launch)
    return p;
}

std::vector<std::string> clean_output(std::vector<std::string> lines, size_t max_line_chars) {
    auto marker = [](const std::string& l) {
        return l.rfind("[xi] started pid ", 0) == 0 || l.rfind("[xi] stopped pid ", 0) == 0;
    };
    if (!lines.empty() && marker(lines.front())) lines.erase(lines.begin());
    if (!lines.empty() && marker(lines.back())) lines.pop_back();
    for (auto& l : lines)
        if (l.size() > max_line_chars) l = l.substr(0, max_line_chars) + " …[line truncated]";
    return lines;
}

json run_job(const Runtime& rt, const ExecRequest& req, const fs::path& work_dir, const fs::path& log_file) {
    const RuntimeSpec* spec = find_spec(rt.id);
    if (!spec) throw std::runtime_error("no spec for runtime " + rt.id);
    std::error_code ec;
    fs::create_directories(work_dir, ec);
    fs::path root = fs::absolute(work_dir, ec);
    fs::path job = root / ".jobs" / random_hex(6);
    fs::create_directories(job, ec);
    if (ec) throw std::runtime_error("cannot create job folder " + xgi::path_to_utf8(job) + ": " + ec.message());

    // Java's single-file launcher wants the file name to match a public class; "Main" is the usual one.
    fs::path source = job / ((spec->id == "java" ? "Main" : "main") + spec->ext);
    fs::path binary = job / ("main" + std::string(xgi::kExeSuffix));
    {
        std::string text = req.code;
        if (spec->id == "cmd") {  // batch files misbehave with bare LF line endings
            std::string crlf;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) crlf += '\r';
                crlf += text[i];
            }
            text = std::move(crlf);
        }
        std::ofstream f(source, std::ios::binary);
        if (spec->id == "powershell") f << "\xEF\xBB\xBF";  // Windows PowerShell 5.1 reads BOM-less files as ANSI
        f << text;
        if (!f) throw std::runtime_error("cannot write " + xgi::path_to_utf8(source));
    }

    ExecPlan plan = build_exec_plan(*spec, rt.exe, source, binary);
    const double started = xgi::mono_now();
    const double deadline = started + req.timeout_s;
    std::string stage = "run";
    RunResult r;
    if (plan.compile) {
        stage = "compile";
        r = run_to_completion("exec-compile", log_file, *plan.compile, job, deadline);
        if (!r.timed_out && r.exit_code && *r.exit_code == 0) {
            stage = "run";
            r = run_to_completion("exec", log_file, plan.run, root, deadline);
        }
    } else {
        r = run_to_completion("exec", log_file, plan.run, root, deadline);
    }
    const double duration = xgi::mono_now() - started;

    fs::remove_all(job, ec);
    if (ec) xgi::log::warn("exec: cannot remove {}: {}", xgi::path_to_utf8(job), ec.message());

    return json{{"language", rt.id},
                {"runtime", rt.version.empty() ? rt.label : rt.version},
                {"stage", stage},
                {"exit_code", r.exit_code ? json(*r.exit_code) : json(nullptr)},
                {"timed_out", r.timed_out},
                {"duration_s", duration},
                {"output", r.lines},
                {"output_truncated", r.truncated}};
}

}  // namespace xi
