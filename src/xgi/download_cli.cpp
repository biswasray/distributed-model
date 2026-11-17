#include "xgi/download_cli.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <map>
#include <set>
#include <thread>

#include "common/cli.hpp"
#include "common/config.hpp"
#include "common/http_util.hpp"
#include "common/hub.hpp"
#include "common/platform.hpp"
#include "common/shutdown.hpp"
#include "common/sysinfo.hpp"
#include "common/util.hpp"
#include "xgi/config_store.hpp"

namespace xgi {

namespace {

const char* kDownloadUsage = R"(xgi download - download model files onto nodes, from Hugging Face or GitHub releases

Usage:
  xgi download files  <hf|github> <owner/repo> --node N [--ref R] [--all]
        List a repo's GGUF files (--all: every file). The node does the lookup, so this
        also checks that it can reach the hub. --ref: branch/commit (HF, default main) or
        release tag (GitHub, default latest).
  xgi download start  <hf|github> <owner/repo> <file>... --node N[,N...] [--ref R] [--dest DIR] [--detach]
  xgi download start  <file URL> --node N[,N...] [--dest DIR] [--detach]
        Start downloading into each node's models folder (under DIR if given) and show
        progress. Ctrl+C pauses; --detach returns at once. A URL is a Hugging Face
        .../blob/<ref>/<file> or .../resolve/<ref>/<file> link, or a GitHub
        .../releases/download/<tag>/<file> link.
  xgi download list   [--node N]                List downloads on every node (or one)
  xgi download watch  <node>/<id>...            Show progress until they finish
  xgi download pause  <node>/<id>...            Stop; the partial file stays for resume
  xgi download resume <node>/<id>...            Continue a paused or failed download
  xgi download cancel <node>/<id>...            Stop and delete the partial file
  xgi download remove <node>/<id>...            Drop a finished download from the list

Node "@xgi" is the XGI server itself: files go to its console.models_dir (default ./models), so
models can be downloaded with no agents running, e.g.  xgi download start <url> --node @xgi

Connection (all commands):
  --coordinator URL   default: http://127.0.0.1:<coordinator.port from cluster.yaml>
  --token T           default: env QWEN_CLUSTER_TOKEN, then cluster_token in cluster.yaml
  --config PATH       cluster.yaml to read those defaults from (same lookup as xgi serve)

The coordinator's web console must be enabled (console.enabled, the default). Gated or
private Hugging Face repos need HF_TOKEN set for the agent on each node (GITHUB_TOKEN for GitHub).
)";

struct Conn {
    std::string url, token;
};

struct JobRef {
    std::string node, id;
    std::string key() const { return node + "/" + id; }
};

bool terminal(const std::string& s) { return s == "completed" || s == "failed" || s == "canceled"; }

std::string err_text(const HttpResult& r) {
    if (r.status == 0) return "coordinator unreachable: " + r.error;
    std::string msg = r.error;
    if (r.body.is_object() && r.body.contains("error") && r.body["error"].is_object())
        msg = r.body["error"].value("message", msg);
    return std::format("{} (HTTP {})", msg, r.status);
}

std::optional<JobRef> parse_ref(const std::string& s) {
    auto slash = s.rfind('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == s.size()) return std::nullopt;
    return JobRef{s.substr(0, slash), s.substr(slash + 1)};
}

double num(const nlohmann::json& j, const char* k) {
    return j.contains(k) && j[k].is_number() ? j[k].get<double>() : 0.0;
}

// GET /console/downloads for the nodes in `refs`: key -> job.
std::map<std::string, nlohmann::json> fetch_jobs(const Conn& c, const std::vector<JobRef>& refs, std::string& error) {
    std::map<std::string, nlohmann::json> out;
    std::set<std::string> nodes;
    for (const auto& r : refs) nodes.insert(r.node);
    for (const auto& n : nodes) {
        auto r = http_call("GET", c.url, "/console/downloads?node_id=" + hub::encode_segment(n), c.token, nullptr, 15);
        if (!r.ok) {
            error = err_text(r);
            continue;
        }
        for (const auto& j : r.body.value("downloads", nlohmann::json::array()))
            out[n + "/" + j.value("id", "")] = j;
    }
    return out;
}

void print_job_summary(const std::string& key, const nlohmann::json& j) {
    std::string state = j.value("state", "?");
    std::string what = std::format("{} {}@{}", j.value("source", ""), j.value("repo", ""), j.value("ref", ""));
    std::cout << std::format("{}  {}  {}  {}", key, state, hub::format_bytes(num(j, "done_bytes")), what);
    if (!j.value("error", "").empty()) std::cout << "\n    error: " << j.value("error", "");
    std::cout << "\n";
    if (state == "completed")
        for (const auto& f : j.value("files", nlohmann::json::array()))
            std::cout << "    " << f.value("target", "") << (f.value("state", "") == "present" ? "  (already there)" : "")
                      << "\n";
}

int watch(const Conn& c, const std::vector<JobRef>& refs) {
    static std::atomic<bool> interrupted{false};
    install_shutdown_handler([] { interrupted = true; });
    std::map<std::string, std::string> last_state;
    size_t last_len = 0;
    auto clear_line = [&] {
        if (last_len) std::cout << "\r" << std::string(last_len, ' ') << "\r";
        last_len = 0;
    };
    int misses = 0;
    while (true) {
        std::string error;
        auto jobs = fetch_jobs(c, refs, error);
        std::vector<std::string> parts;
        bool all_done = true, any_bad = false;
        for (const auto& r : refs) {
            auto it = jobs.find(r.key());
            if (it == jobs.end()) {
                all_done = all_done && error.empty() && misses > 2;  // gone (removed, or the agent restarted)
                continue;
            }
            const auto& j = it->second;
            std::string st = j.value("state", "?");
            if (last_state[r.key()] != st && (terminal(st) || st == "paused")) {
                clear_line();
                print_job_summary(r.key(), j);
            }
            last_state[r.key()] = st;
            if (!terminal(st)) all_done = false;
            if (st == "failed" || st == "canceled") any_bad = true;
            if (!terminal(st)) parts.push_back(progress_line(j, refs.size() == 1 ? 24 : 0));
        }
        if (jobs.empty()) ++misses;
        if (all_done) {
            clear_line();
            return any_bad ? 1 : 0;
        }
        std::string line = !error.empty() && parts.empty() ? "waiting: " + error : join(parts, " | ");
        if (line.size() > 200) line = line.substr(0, 200);
        clear_line();
        std::cout << line << std::flush;
        last_len = line.size();

        for (int i = 0; i < 10 && !interrupted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (interrupted) {
            clear_line();
            std::cout << "Pausing...\n";
            for (const auto& r : refs) {
                if (terminal(last_state[r.key()]) || last_state[r.key()] == "paused") continue;
                auto res = http_call("POST", c.url,
                                     "/console/downloads/" + hub::encode_segment(r.node) + "/" + r.id + "/pause", c.token,
                                     nlohmann::json::object(), 15);
                if (res.ok) std::cout << "  paused " << r.key() << ": resume with  xgi download resume " << r.key() << "\n";
                else std::cout << "  " << r.key() << ": " << err_text(res) << "\n";
            }
            return 130;
        }
    }
}

}  // namespace

std::string progress_line(const nlohmann::json& j, int bar_width) {
    const std::string node = j.value("node_id", "");
    const std::string state = j.value("state", "?");
    const double done = num(j, "done_bytes");
    const bool known = j.contains("total_bytes") && j["total_bytes"].is_number();
    const double total = known ? j["total_bytes"].get<double>() : 0;
    const double pct = known && total > 0 ? std::min(100.0, done * 100.0 / total) : 0;
    std::string out = node;
    if (bar_width > 0) {
        int fill = known ? static_cast<int>(pct / 100.0 * bar_width + 0.5) : 0;
        out += "  [" + std::string(static_cast<size_t>(fill), '#') + std::string(static_cast<size_t>(bar_width - fill), '.') + "]";
    }
    out += known ? std::format(" {:5.1f}%", pct) : std::string("    ?%");
    if (bar_width > 0) {
        out += "  " + hub::format_bytes(done);
        if (known) out += " / " + hub::format_bytes(total);
    }
    if (state == "downloading") {
        out += "  " + hub::format_bytes(num(j, "speed_bps")) + "/s";
        if (bar_width > 0 && j.contains("eta_s") && j["eta_s"].is_number())
            out += "  ETA " + hub::format_duration(j["eta_s"].get<double>());
    } else {
        out += "  " + state;
    }
    const auto files = j.value("files", nlohmann::json::array());
    if (bar_width > 0 && files.size() > 1)
        out += std::format("  file {}/{}", j.value("current_file", 0) + 1, files.size());
    return out;
}

int run_download_cli(int argc, char** argv) {
    CliArgs args(argc, argv, {"--help", "-h", "--all", "--detach"});
    std::vector<std::string> pos(args.positional().begin() + 1, args.positional().end());  // drop "download"
    if (args.has("--help") || args.has("-h") || pos.empty()) {
        std::cout << kDownloadUsage;
        return pos.empty() && !args.has("--help") && !args.has("-h") ? 2 : 0;
    }
    if (auto bad = args.unknown({"--coordinator", "--token", "--config", "--node", "--ref", "--dest", "--all", "--detach",
                                 "--help", "-h"});
        !bad.empty()) {
        std::cerr << "xgi download: unknown option " << bad.front() << "\n\n" << kDownloadUsage;
        return 2;
    }
    const std::string cmd = pos.front();
    pos.erase(pos.begin());

    // Where the coordinator is, and its token: flags, then the environment, then cluster.yaml.
    Conn c;
    std::optional<ClusterConfig> cfg;
    {
        const auto exe = sysinfo::executable_path();
        auto where = locate_config(args.get("--config"), exe.empty() ? exe : exe.parent_path());
        if (where.found) {
            try {
                cfg = load_config(where.path);
            } catch (const std::exception& e) {
                if (!args.has("--coordinator") || !args.has("--token")) std::cerr << "xgi download: warning: " << e.what() << "\n";
            }
        }
    }
    std::string host = "127.0.0.1";
    int port = 8100;
    if (cfg) {
        port = cfg->coordinator.port;
        if (!cfg->coordinator.host.empty() && cfg->coordinator.host != "0.0.0.0") host = cfg->coordinator.host;
    }
    c.url = args.get_or("--coordinator", std::format("http://{}:{}", host, port));
    if (auto t = args.get("--token")) c.token = *t;
    else if (auto e = get_env(kTokenEnv)) c.token = *e;
    else if (cfg) c.token = cfg->cluster_token;
    if (!parse_base_url(c.url)) {
        std::cerr << "xgi download: --coordinator must be an http://host:port URL\n";
        return 2;
    }

    auto need_refs = [&](std::vector<JobRef>& out) {
        for (const auto& p : pos) {
            auto r = parse_ref(p);
            if (!r) {
                std::cerr << "xgi download: '" << p << "' is not <node>/<id> (see xgi download list)\n";
                return false;
            }
            out.push_back(*r);
        }
        if (out.empty()) std::cerr << "xgi download " << cmd << ": name at least one <node>/<id>\n";
        return !out.empty();
    };

    if (cmd == "files") {
        if (pos.size() != 2 || !args.has("--node")) {
            std::cerr << "usage: xgi download files <hf|github> <owner/repo> --node N [--ref R] [--all]\n";
            return 2;
        }
        auto src = hub::parse_source(pos[0]);
        if (!src) {
            std::cerr << "xgi download: source must be hf (huggingface) or github\n";
            return 2;
        }
        std::string path = std::format("/console/hub/files?node_id={}&source={}&repo={}&ref={}&all={}",
                                       hub::encode_segment(args.get_or("--node", "")), hub::source_name(*src),
                                       hub::encode_segment(pos[1]), hub::encode_segment(args.get_or("--ref", "")),
                                       args.has("--all") ? "true" : "false");
        auto r = http_call("GET", c.url, path, c.token, nullptr, 90);
        if (!r.ok) {
            std::cerr << "xgi download: " << err_text(r) << "\n";
            return 1;
        }
        const auto files = r.body.value("files", nlohmann::json::array());
        std::cout << std::format("{} {}@{}: {} {}file(s) (of {})\n", hub::source_name(*src), pos[1],
                                 r.body.value("ref", ""), files.size(), args.has("--all") ? "" : "GGUF ",
                                 r.body.value("total_files", 0));
        for (const auto& f : files)
            std::cout << std::format("  {:>10}  {}\n",
                                     f["size_bytes"].is_number() ? hub::format_bytes(f["size_bytes"].get<double>()) : "?",
                                     f.value("path", ""));
        return 0;
    }

    if (cmd == "start") {
        nlohmann::json body;
        if (pos.size() == 1) {
            auto ref = hub::parse_hub_url(pos[0]);
            if (!ref) {
                std::cerr << "xgi download: not a Hugging Face file link or GitHub release asset link: " << pos[0] << "\n";
                return 2;
            }
            body = {{"source", hub::source_name(ref->source)}, {"repo", ref->repo}, {"ref", ref->ref},
                    {"files", nlohmann::json::array({ref->file})}};
        } else if (pos.size() >= 3) {
            auto src = hub::parse_source(pos[0]);
            if (!src) {
                std::cerr << "xgi download: source must be hf (huggingface) or github\n";
                return 2;
            }
            body = {{"source", hub::source_name(*src)}, {"repo", pos[1]}, {"ref", args.get_or("--ref", "")},
                    {"files", std::vector<std::string>(pos.begin() + 2, pos.end())}};
        } else {
            std::cerr << "usage: xgi download start <hf|github> <owner/repo> <file>... --node N  (or a file URL)\n";
            return 2;
        }
        std::vector<std::string> nodes;
        for (const auto& n : split(args.get_or("--node", ""), ','))
            if (!n.empty()) nodes.push_back(n);
        if (nodes.empty()) {
            std::cerr << "xgi download start: --node is required (comma-separate several)\n";
            return 2;
        }
        body["node_ids"] = nodes;
        if (auto d = args.get("--dest")) body["dest_dir"] = *d;
        auto r = http_call("POST", c.url, "/console/downloads", c.token, body, 60);
        std::vector<JobRef> refs;
        if (r.body.is_object()) {
            for (const auto& j : r.body.value("started", nlohmann::json::array())) {
                refs.push_back({j.value("node_id", ""), j.value("id", "")});
                std::cout << "started " << refs.back().key() << "  " << j.value("repo", "") << " -> "
                          << j.value("files", nlohmann::json::array()).size() << " file(s)\n";
            }
            for (const auto& e : r.body.value("errors", nlohmann::json::array()))
                std::cerr << "  " << e.value("node_id", "?") << ": " << e.value("error", "failed") << "\n";
        }
        if (refs.empty()) {
            if (r.status == 0 || !r.body.is_object() || !r.body.contains("errors"))
                std::cerr << "xgi download: " << err_text(r) << "\n";
            return 1;
        }
        if (args.has("--detach")) return 0;
        return watch(c, refs);
    }

    if (cmd == "list") {
        std::string path = "/console/downloads";
        if (auto n = args.get("--node")) path += "?node_id=" + hub::encode_segment(*n);
        auto r = http_call("GET", c.url, path, c.token, nullptr, 20);
        if (!r.ok) {
            std::cerr << "xgi download: " << err_text(r) << "\n";
            return 1;
        }
        const auto jobs = r.body.value("downloads", nlohmann::json::array());
        if (jobs.empty()) std::cout << "no downloads\n";
        for (const auto& j : jobs) {
            std::string files;
            for (const auto& f : j.value("files", nlohmann::json::array()))
                files += (files.empty() ? "" : ", ") + f.value("target", "");
            std::cout << std::format("{}/{}  {:<11} {}  {} {}@{}\n    {}\n", j.value("node_id", ""), j.value("id", ""),
                                     j.value("state", ""), progress_line(j, 0).substr(j.value("node_id", "").size()),
                                     j.value("source", ""), j.value("repo", ""), j.value("ref", ""), files);
            if (!j.value("error", "").empty()) std::cout << "    error: " << j.value("error", "") << "\n";
        }
        for (const auto& e : r.body.value("errors", nlohmann::json::array()))
            std::cerr << "  " << e.value("node_id", "?") << ": " << e.value("error", "") << "\n";
        return 0;
    }

    if (cmd == "watch") {
        std::vector<JobRef> refs;
        if (!need_refs(refs)) return 2;
        return watch(c, refs);
    }

    if (cmd == "pause" || cmd == "resume" || cmd == "cancel" || cmd == "remove") {
        std::vector<JobRef> refs;
        if (!need_refs(refs)) return 2;
        int rc = 0;
        for (const auto& ref : refs) {
            std::string path = "/console/downloads/" + hub::encode_segment(ref.node) + "/" + ref.id;
            auto r = cmd == "remove" ? http_call("DELETE", c.url, path, c.token, nullptr, 20)
                                     : http_call("POST", c.url, path + "/" + cmd, c.token, nlohmann::json::object(), 20);
            if (!r.ok) {
                std::cerr << ref.key() << ": " << err_text(r) << "\n";
                rc = 1;
                continue;
            }
            std::cout << ref.key() << ": " << (cmd == "remove" ? std::string("removed") : r.body.value("state", "ok"))
                      << "\n";
        }
        return rc;
    }

    std::cerr << "xgi download: unknown command '" << cmd << "'\n\n" << kDownloadUsage;
    return 2;
}

}  // namespace xgi
