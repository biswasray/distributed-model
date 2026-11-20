#include "xi/downloads.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <random>
#include <sstream>

#include "common/http_util.hpp"
#include "common/log.hpp"
#include "common/platform.hpp"
#include "common/util.hpp"
#include "xi/process.hpp"
#include "xi/runtimes.hpp"

namespace xi {

namespace hub = xgi::hub;

namespace {

constexpr double kPollS = 0.25;
constexpr int kMaxRetries = 5;
constexpr double kListTimeoutS = 60;
constexpr int kBusyWaitS = 15;  // a peer answered 429: it is sending its maximum number of files

std::string new_id() {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    std::string out;
    for (int i = 0; i < 4; ++i) out += std::format("{:02x}", (rd() ^ counter.fetch_add(7)) & 0xff);
    return out;
}

bool terminal(const std::string& s) { return s == "completed" || s == "failed" || s == "canceled"; }
bool active(const std::string& s) { return s == "starting" || s == "downloading" || s == "pausing" || s == "canceling"; }

std::uint64_t file_size_or_zero(const fs::path& p) {
    std::error_code ec;
    auto n = fs::file_size(p, ec);
    return ec ? 0 : static_cast<std::uint64_t>(n);
}

fs::path part_of(const fs::path& target) {
    fs::path p = target;
    p += ".part";
    return p;
}

std::string token_for(hub::Source s) {
    const char* names[2] = {s == hub::Source::huggingface ? "HF_TOKEN" : "GITHUB_TOKEN",
                            s == hub::Source::huggingface ? "HUGGING_FACE_HUB_TOKEN" : "GH_TOKEN"};
    for (const char* n : names)
        if (auto v = xgi::get_env(n); v && !v->empty()) return *v;
    return {};
}

struct CurlRun {
    std::optional<long long> exit_code;
    bool timed_out = false;
    std::vector<std::string> lines;
};

}  // namespace

// ------------------------------------------------------------------------------------------ helpers

namespace {

std::string parse_dest_dir(const json& j) {
    std::string d;
    if (j.contains("dest_dir") && !j["dest_dir"].is_null()) {
        if (!j["dest_dir"].is_string()) throw std::invalid_argument("dest_dir must be a string");
        d = j["dest_dir"];
        while (!d.empty() && d.back() == '/') d.pop_back();
        if (!d.empty() && !hub::valid_file_path(d, true))
            throw std::invalid_argument("dest_dir must be a relative folder inside the models folder");
    }
    return d;
}

// source "peer" (ADR-012): files are .gguf paths in the peer's models folder.
DownloadRequest peer_request(const json& j) {
    DownloadRequest r;
    r.peer = true;
    const std::string url = j.contains("peer_url") && j["peer_url"].is_string() ? j["peer_url"].get<std::string>() : "";
    if (url == hub::kXgiPeer) {
        r.peer_url = url;
    } else if (auto n = hub::normalize_peer_url(url)) {
        r.peer_url = *n;
    } else {
        throw std::invalid_argument("peer_url must be \"@xgi\" or http://<LAN IPv4 address>:<port> (10.x, 172.16-31.x, "
                                    "192.168.x, 127.x or 169.254.x)");
    }
    if (j.contains("peer_name") && j["peer_name"].is_string()) r.peer_name = j["peer_name"];
    if (r.peer_name.size() > 128 ||
        std::any_of(r.peer_name.begin(), r.peer_name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
        throw std::invalid_argument("peer_name must be at most 128 printable characters");
    if (r.peer_name.empty()) r.peer_name = r.peer_url;
    r.dest_dir = parse_dest_dir(j);
    if (!j.contains("files") || !j["files"].is_array() || j["files"].empty())
        throw std::invalid_argument("files must be a non-empty array of file paths");
    if (j["files"].size() > DownloadRequest::kMaxFiles)
        throw std::invalid_argument(std::format("at most {} files per download", DownloadRequest::kMaxFiles));
    for (const auto& f : j["files"]) {
        if (!f.is_string() || !hub::valid_file_path(f.get<std::string>(), true) || !hub::is_gguf(f.get<std::string>()))
            throw std::invalid_argument("invalid file path " + f.dump() + " (a peer copy is a .gguf path in its models folder)");
        if (std::find(r.files.begin(), r.files.end(), f.get<std::string>()) == r.files.end()) r.files.push_back(f);
    }
    return r;
}

}  // namespace

DownloadRequest DownloadRequest::from_json(const json& j) {
    if (!j.is_object()) throw std::invalid_argument("body must be a JSON object");
    if (j.contains("source") && j["source"].is_string()) {
        const std::string s = xgi::to_lower(j["source"].get<std::string>());
        if (s == "peer" || s == "lan") return peer_request(j);
    }
    DownloadRequest r;
    auto src = j.contains("source") && j["source"].is_string() ? hub::parse_source(j["source"].get<std::string>())
                                                                 : std::nullopt;
    if (!src) throw std::invalid_argument("source must be \"huggingface\", \"github\" or \"peer\"");
    r.source = *src;
    if (!j.contains("repo") || !j["repo"].is_string() || !hub::valid_repo(j["repo"].get<std::string>()))
        throw std::invalid_argument("repo must be \"owner/name\"");
    r.repo = j["repo"];
    r.ref = j.contains("ref") && j["ref"].is_string() ? j["ref"].get<std::string>() : "";
    if (r.ref.empty()) r.ref = hub::default_ref(r.source);
    if (!hub::valid_ref(r.ref)) throw std::invalid_argument("ref is not a valid branch, tag or commit");
    r.dest_dir = parse_dest_dir(j);
    if (!j.contains("files") || !j["files"].is_array() || j["files"].empty())
        throw std::invalid_argument("files must be a non-empty array of file paths");
    if (j["files"].size() > kMaxFiles) throw std::invalid_argument(std::format("at most {} files per download", kMaxFiles));
    for (const auto& f : j["files"]) {
        if (!f.is_string() || !hub::valid_file_path(f.get<std::string>(), r.source == hub::Source::huggingface))
            throw std::invalid_argument("invalid file path " + f.dump());
        if (std::find(r.files.begin(), r.files.end(), f.get<std::string>()) == r.files.end()) r.files.push_back(f);
    }
    return r;
}

std::optional<fs::path> download_target(const fs::path& models_dir, const std::string& dest_dir, const std::string& file,
                                        bool keep_dirs) {
    if (!dest_dir.empty() && !hub::valid_file_path(dest_dir, true)) return std::nullopt;
    if (!hub::valid_file_path(file, true)) return std::nullopt;
    std::string name = keep_dirs ? file : file.substr(file.rfind('/') == std::string::npos ? 0 : file.rfind('/') + 1);
    fs::path p = models_dir;
    if (!dest_dir.empty()) p /= xgi::path_from_utf8(dest_dir);
    p /= xgi::path_from_utf8(name);
    return p.lexically_normal();
}

CurlOutcome parse_curl_output(const std::vector<std::string>& lines) {
    CurlOutcome o;
    for (auto l : lines) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.rfind("curl: (", 0) == 0) o.error = l.substr(6);
        // -w "%{http_code}\n" prints the final status on its own line ("000" = no response).
        if (l.size() == 3 && std::all_of(l.begin(), l.end(), [](char c) { return c >= '0' && c <= '9'; }))
            o.http_status = std::stoi(l);
    }
    return o;
}

// ------------------------------------------------------------------------------------------ Downloads

Downloads::Downloads(std::function<Paths()> paths, std::function<void()> on_file_done)
    : paths_(std::move(paths)), on_file_done_(std::move(on_file_done)), bases_(hub::Bases::from_env()) {
    if (auto c = find_on_path("curl")) {
        curl_ = *c;
        try {
            ManagedProcess p("curl-version", paths_().log_dir / "downloads.log");
            p.start({xgi::path_to_utf8(curl_), "--version"});
            const double until = xgi::mono_now() + 5;
            while (p.info().state == "running" && xgi::mono_now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            p.stop(0);
            curl_version_ = first_line(clean_output(p.tail(5)));
        } catch (const std::exception& e) {
            xgi::log::warn("downloads: curl --version failed: {}", e.what());
        }
    }
    if (curl_.empty()) xgi::log::warn("downloads: curl not found on PATH; model downloads are unavailable on this node");
}

Downloads::~Downloads() { shutdown(); }

json Downloads::tool_json() const {
    return json{{"available", available()}, {"tool", curl_version_.empty() ? json(nullptr) : json(curl_version_)}};
}

std::shared_ptr<Downloads::Job> Downloads::find(const std::string& id) {
    auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second;
}

json Downloads::job_json(const Job& j) const {
    json files = json::array();
    std::uint64_t done = 0, total = 0;
    bool total_known = true;
    for (const auto& f : j.files) {
        files.push_back({{"path", f.path},
                         {"target", f.target_rel},
                         {"size_bytes", f.size ? json(*f.size) : json(nullptr)},
                         {"done_bytes", f.done},
                         {"state", f.state}});
        done += f.done;
        if (f.size) total += *f.size;
        else total_known = false;
    }
    json eta = nullptr;
    if (j.state == "downloading" && total_known && j.speed_bps > 1 && total >= done)
        eta = static_cast<double>(total - done) / j.speed_bps;
    return json{{"id", j.id},
                {"source", j.source},
                {"repo", j.repo},
                {"ref", j.ref},
                {"dest_dir", j.dest_dir},
                {"peer", j.peer ? json(j.peer_name) : json(nullptr)},
                {"peer_url", j.peer ? json(j.peer_url) : json(nullptr)},
                {"state", j.state},
                {"error", j.error},
                {"note", j.note},
                {"files", files},
                {"current_file", j.current},
                {"done_bytes", done},
                {"total_bytes", total_known ? json(total) : json(nullptr)},
                {"speed_bps", j.state == "downloading" ? j.speed_bps : 0.0},
                {"eta_s", eta},
                {"created_at", j.created_at},
                {"updated_at", j.updated_at},
                {"finished_at", j.finished_at > 0 ? json(j.finished_at) : json(nullptr)}};
}

json Downloads::list() {
    std::lock_guard lk(mu_);
    std::vector<std::shared_ptr<Job>> v;
    for (auto& [id, j] : jobs_) v.push_back(j);
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a->created_at > b->created_at; });
    json arr = json::array();
    for (auto& j : v) arr.push_back(job_json(*j));
    return json{{"downloads", arr}, {"available", available()}};
}

void Downloads::prune() {
    if (jobs_.size() <= kMaxKept) return;
    std::vector<std::shared_ptr<Job>> done;
    for (auto& [id, j] : jobs_)
        if (!j->running && terminal(j->state)) done.push_back(j);
    std::sort(done.begin(), done.end(), [](auto& a, auto& b) { return a->created_at < b->created_at; });
    for (auto& j : done) {
        if (jobs_.size() <= kMaxKept) break;
        if (j->thread.joinable()) j->thread.join();  // already finished (running == false)
        jobs_.erase(j->id);
    }
}

json Downloads::start(const DownloadRequest& r) {
    if (!available()) throw DownloadError(503, "unavailable", "curl is not installed on this node, so it cannot download");
    const Paths p = paths_();
    auto j = std::make_shared<Job>();
    j->id = new_id();
    j->src = r.source;
    j->source = r.peer ? "peer" : hub::source_name(r.source);
    j->repo = r.peer ? "" : r.repo;
    j->ref = r.peer ? "" : r.ref;
    j->dest_dir = r.dest_dir;
    j->peer = r.peer;
    j->peer_name = r.peer_name;
    j->peer_url = r.peer_url;
    if (r.peer && r.peer_url == hub::kXgiPeer) {  // the XGI machine: this node's coordinator
        auto u = xgi::parse_base_url(p.coordinator);
        if (!u) throw DownloadError(400, "invalid_request", "this machine has no coordinator URL, so it cannot copy from @xgi");
        j->peer_url = u->origin();
    }
    j->created_at = j->updated_at = xgi::wall_now();
    for (const auto& f : r.files) {
        auto t = download_target(p.models_dir, r.dest_dir, f, r.peer);
        if (!t) throw DownloadError(400, "invalid_request", "invalid file path " + f);
        File file;
        file.path = f;
        file.target = *t;
        file.target_rel = xgi::generic_utf8(t->lexically_relative(p.models_dir.lexically_normal()));
        for (const auto& g : j->files)
            if (g.target == file.target)
                throw DownloadError(400, "invalid_request", "two files would be saved as " + file.target_rel);
        j->files.push_back(std::move(file));
    }

    std::lock_guard lk(mu_);
    if (shutting_down_) throw DownloadError(503, "unavailable", "the agent is shutting down");
    size_t running = 0;
    for (auto& [id, o] : jobs_) {
        if (active(o->state)) ++running;
        if (terminal(o->state)) continue;
        for (const auto& of : o->files)
            for (const auto& nf : j->files)
                if (of.target == nf.target)
                    throw DownloadError(409, "conflict",
                                        std::format("{} is already being downloaded by job {} ({}); resume or cancel it",
                                                    nf.target_rel, o->id, o->state));
    }
    if (running >= kMaxActive)
        throw DownloadError(429, "capacity", std::format("{} downloads are already running on this node", running));
    jobs_[j->id] = j;
    if (j->peer)
        xgi::log::info("download {}: LAN copy from {} ({}) -> {} file(s) into {}", j->id, j->peer_name, j->peer_url,
                       j->files.size(), xgi::path_to_utf8(p.models_dir));
    else
        xgi::log::info("download {}: {} {}@{} -> {} file(s) into {}", j->id, j->source, j->repo, j->ref, j->files.size(),
                       xgi::path_to_utf8(p.models_dir));
    launch(j);
    prune();
    return job_json(*j);
}

void Downloads::launch(const std::shared_ptr<Job>& j) {
    if (j->thread.joinable()) j->thread.join();  // the previous run has finished (running == false)
    j->stop.clear();
    j->error.clear();
    j->finished_at = 0;
    j->speed_bps = 0;
    j->note.clear();
    j->state = j->listed ? "downloading" : "starting";
    j->running = true;
    j->thread = std::thread([this, j] { run(j); });
}

json Downloads::pause(const std::string& id) {
    std::shared_ptr<ManagedProcess> proc;
    json out;
    {
        std::lock_guard lk(mu_);
        auto j = find(id);
        if (!j) throw DownloadError(404, "not_found", "no download " + id);
        if (j->state == "paused" || j->state == "pausing") return job_json(*j);
        if (j->state != "starting" && j->state != "downloading")
            throw DownloadError(409, "conflict", "download " + id + " is " + j->state + ", not running");
        j->stop = "pause";
        j->state = "pausing";
        j->updated_at = xgi::wall_now();
        proc = j->proc;
        out = job_json(*j);
    }
    if (proc) proc->stop(0);
    xgi::log::info("download {}: pause requested", id);
    return out;
}

json Downloads::resume(const std::string& id) {
    std::lock_guard lk(mu_);
    auto j = find(id);
    if (!j) throw DownloadError(404, "not_found", "no download " + id);
    if (active(j->state)) return job_json(*j);
    if (j->running) throw DownloadError(409, "conflict", "download " + id + " is still stopping; try again");
    if (j->state != "paused" && j->state != "failed")
        throw DownloadError(409, "conflict", "download " + id + " is " + j->state + "; only paused or failed downloads resume");
    if (!available()) throw DownloadError(503, "unavailable", "curl is not installed on this node");
    size_t running = 0;
    for (auto& [oid, o] : jobs_)
        if (active(o->state)) ++running;
    if (running >= kMaxActive)
        throw DownloadError(429, "capacity", std::format("{} downloads are already running on this node", running));
    xgi::log::info("download {}: resuming", id);
    launch(j);
    return job_json(*j);
}

json Downloads::cancel(const std::string& id) {
    std::shared_ptr<ManagedProcess> proc;
    json out;
    {
        std::lock_guard lk(mu_);
        auto j = find(id);
        if (!j) throw DownloadError(404, "not_found", "no download " + id);
        if (j->state == "canceled" || j->state == "canceling") return job_json(*j);
        if (j->state == "completed") throw DownloadError(409, "conflict", "download " + id + " has already completed");
        j->updated_at = xgi::wall_now();
        if (j->running) {
            j->stop = "cancel";
            j->state = "canceling";
            proc = j->proc;
        } else {  // paused or failed: nothing is running, delete the partial files now
            std::error_code ec;
            for (auto& f : j->files)
                if (f.state != "done" && f.state != "present") {
                    fs::remove(part_of(f.target), ec);
                    f.done = 0;
                }
            j->state = "canceled";
            j->finished_at = xgi::wall_now();
        }
        out = job_json(*j);
    }
    if (proc) proc->stop(0);
    xgi::log::info("download {}: canceled", id);
    return out;
}

void Downloads::remove(const std::string& id) {
    std::shared_ptr<Job> j;
    {
        std::lock_guard lk(mu_);
        j = find(id);
        if (!j) throw DownloadError(404, "not_found", "no download " + id);
        if (j->running || active(j->state))
            throw DownloadError(409, "conflict", "download " + id + " is running; pause or cancel it first");
        jobs_.erase(id);
    }
    if (j->thread.joinable()) j->thread.join();
}

void Downloads::shutdown() {
    std::vector<std::shared_ptr<Job>> all;
    std::vector<std::shared_ptr<ManagedProcess>> procs;
    {
        std::lock_guard lk(mu_);
        shutting_down_ = true;
        for (auto& [id, j] : jobs_) {
            all.push_back(j);
            if (j->running) {
                if (j->stop.empty()) j->stop = "shutdown";
                if (j->proc) procs.push_back(j->proc);
            }
        }
    }
    for (auto& p : procs) p->stop(0);
    for (auto& j : all)
        if (j->thread.joinable()) j->thread.join();
}

bool Downloads::stop_requested(const std::shared_ptr<Job>& j) {
    std::lock_guard lk(mu_);
    return !j->stop.empty();
}

namespace {

// Writes one header line to a short-lived file curl reads with -H @file: in a file, not on the
// command line, so other users can't read the secret from the process list.
std::vector<std::string> header_file_args(const std::string& line, const fs::path& dir, std::string& header_file) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path p = dir / ("hdr-" + new_id() + ".txt");
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << "";
    }
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << line << "\n";
    header_file = xgi::path_to_utf8(p);
    return {"-H", "@" + header_file};
}

}  // namespace

std::vector<std::string> Downloads::auth_args(hub::Source s, const fs::path& dir, std::string& header_file) const {
    header_file.clear();
    std::string token = token_for(s);
    if (token.empty()) return {};
    return header_file_args("Authorization: Bearer " + token, dir, header_file);
}

std::vector<std::string> Downloads::auth_args(const Job& j, const fs::path& dir, std::string& header_file) const {
    header_file.clear();
    if (!j.peer) return auth_args(j.src, dir, header_file);
    const std::string token = paths_().token;
    if (token.empty()) return {};
    return header_file_args("X-Cluster-Token: " + token, dir, header_file);
}

namespace {

// Hubs redirect to their CDNs. A peer never needs to, and must not: curl would send the cluster
// token header to wherever the redirect points.
std::vector<std::string> base_args(const fs::path& curl, bool follow = true) {
    std::vector<std::string> a{xgi::path_to_utf8(curl), "-sS"};
    if (follow) a.insert(a.end(), {"-L", "--proto", "=https,http", "--proto-redir", "=https,http"});
    else a.insert(a.end(), {"--proto", "=http"});
    a.insert(a.end(), {"--connect-timeout", "20", "-A", std::string("xi/") + xgi::kVersion});
    return a;
}

// Runs curl until it exits, `deadline` passes, or `tick` returns false (then it is killed).
CurlRun run_curl(const std::vector<std::string>& argv, const fs::path& log_file, double deadline,
                 const std::function<void(std::shared_ptr<ManagedProcess>)>& publish,
                 const std::function<bool()>& tick) {
    auto proc = std::make_shared<ManagedProcess>("download", log_file);
    if (publish) publish(proc);
    CurlRun r;
    try {
        proc->start(argv);
    } catch (...) {
        if (publish) publish(nullptr);
        throw;
    }
    double next_tick = xgi::mono_now() + kPollS;
    while (true) {
        ProcInfo i = proc->info();
        if (i.state != "running") {
            r.exit_code = i.exit_code;
            break;
        }
        if (deadline > 0 && xgi::mono_now() >= deadline) {
            r.timed_out = true;
            break;
        }
        if (xgi::mono_now() >= next_tick) {
            next_tick += kPollS;
            if (tick && !tick()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    proc->stop(0);
    if (tick) tick();
    r.lines = clean_output(proc->tail(50));
    if (publish) publish(nullptr);
    return r;
}

}  // namespace

json Downloads::hub_files(hub::Source s, const std::string& repo, const std::string& ref_in, bool all) {
    if (!available()) throw DownloadError(503, "unavailable", "curl is not installed on this node");
    if (!hub::valid_repo(repo)) throw DownloadError(400, "invalid_request", "repo must be \"owner/name\"");
    std::string ref = ref_in.empty() ? hub::default_ref(s) : ref_in;
    if (!hub::valid_ref(ref)) throw DownloadError(400, "invalid_request", "ref is not a valid branch, tag or commit");
    const Paths p = paths_();
    const fs::path dir = p.log_dir / "downloads";
    std::string hdr;
    auto argv = base_args(curl_);
    auto auth = auth_args(s, dir, hdr);
    argv.insert(argv.end(), auth.begin(), auth.end());
    if (s == hub::Source::github) argv.insert(argv.end(), {"-H", "Accept: application/vnd.github+json"});
    const std::string url = hub::listing_url(bases_, s, repo, ref);
    std::pair<int, json> got;
    try {
        got = fetch_json(argv, url, p);
    } catch (...) {
        std::error_code ec;
        if (!hdr.empty()) fs::remove(xgi::path_from_utf8(hdr), ec);
        throw;
    }
    std::error_code ec;
    if (!hdr.empty()) fs::remove(xgi::path_from_utf8(hdr), ec);
    CurlOutcome o;
    o.http_status = got.first;
    json& body = got.second;
    if (o.http_status != 200) {
        std::string msg = hub::hub_error_message(body);
        if (msg.empty()) msg = std::format("HTTP {}", o.http_status);
        bool missing = o.http_status == 404 || (o.http_status == 401 && token_for(s).empty());
        // Hugging Face answers 401 "Invalid username or password." for a repo that doesn't exist.
        if (missing && s == hub::Source::huggingface)
            msg = "repository or revision not found, or it is private or gated (that needs HF_TOKEN in the agent's "
                  "environment)";
        else if (missing)
            msg += " (no such repository or release)";
        throw DownloadError(missing ? 404 : 502, missing ? "not_found" : "upstream",
                            std::format("{} {}@{}: {}", hub::source_name(s), repo, ref, msg));
    }
    hub::Listing l;
    try {
        l = hub::parse_listing(s, body, ref);
    } catch (const std::exception& e) {
        throw DownloadError(502, "upstream", e.what());
    }
    const size_t total = l.files.size();
    if (!all) std::erase_if(l.files, [](const hub::HubFile& f) { return !hub::is_gguf(f.path); });
    json j = l.to_json();
    j["source"] = hub::source_name(s);
    j["repo"] = repo;
    j["total_files"] = total;
    return j;
}

std::pair<int, json> Downloads::fetch_json(std::vector<std::string> argv, const std::string& url, const Paths& p) {
    const fs::path dir = p.log_dir / "downloads";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path out = dir / ("list-" + new_id() + ".json");
    argv.insert(argv.end(), {"--max-time", "50", "-o", xgi::path_to_utf8(out), "-w", "%{http_code}\\n", url});
    CurlRun r = run_curl(argv, p.log_dir / "downloads.log", xgi::mono_now() + kListTimeoutS, nullptr, nullptr);
    std::string text;
    {
        std::ifstream f(out, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    fs::remove(out, ec);
    CurlOutcome o = parse_curl_output(r.lines);
    if (r.timed_out) throw DownloadError(504, "upstream", url + " did not answer within 60 s");
    if (!r.exit_code || *r.exit_code != 0)
        throw DownloadError(502, "upstream", "cannot reach " + url + ": " + (o.error.empty() ? "curl failed" : o.error));
    return {o.http_status, json::parse(text, nullptr, false)};
}

json Downloads::peer_files(const std::string& base, const std::string& token) {
    const Paths p = paths_();
    const fs::path dir = p.log_dir / "downloads";
    std::string hdr;
    auto argv = base_args(curl_, false);
    std::vector<std::string> auth;
    if (!token.empty()) auth = header_file_args("X-Cluster-Token: " + token, dir, hdr);
    argv.insert(argv.end(), auth.begin(), auth.end());
    const std::string url = hub::peer_listing_url(base);
    std::pair<int, json> got;
    try {
        got = fetch_json(argv, url, p);
    } catch (...) {
        std::error_code ec;
        if (!hdr.empty()) fs::remove(xgi::path_from_utf8(hdr), ec);
        throw;
    }
    std::error_code ec;
    if (!hdr.empty()) fs::remove(xgi::path_from_utf8(hdr), ec);
    const int status = got.first;
    if (status == 401)
        throw DownloadError(502, "upstream", "the peer " + base + " rejected the cluster token (HTTP 401): both need the same token");
    if (status == 404)
        throw DownloadError(502, "upstream",
                            "the peer " + base + " does not serve model files (HTTP 404); update XI/XGI there (ADR-012)");
    if (status != 200) {
        std::string msg = hub::hub_error_message(got.second.is_object() && got.second.contains("error") &&
                                                         got.second["error"].is_object()
                                                     ? got.second["error"]
                                                     : got.second);
        throw DownloadError(502, "upstream",
                            std::format("the peer {} answered HTTP {}{}", base, status, msg.empty() ? "" : ": " + msg));
    }
    try {
        return hub::parse_peer_listing(got.second).to_json();
    } catch (const std::exception& e) {
        throw DownloadError(502, "upstream", e.what());
    }
}

void Downloads::finish(const std::shared_ptr<Job>& j, const std::string& state, const std::string& error) {
    std::lock_guard lk(mu_);
    j->state = state;
    j->error = error;
    j->speed_bps = 0;
    j->finished_at = j->updated_at = xgi::wall_now();
}

void Downloads::run(std::shared_ptr<Job> j) {
    const Paths p = paths_();
    std::string fail;
    bool stopped = false;
    try {
        if (!j->listed) {
            // Resolve the ref (GitHub "latest") and learn the sizes; this also checks the files exist.
            json l = j->peer ? peer_files(j->peer_url, p.token) : hub_files(j->src, j->repo, j->ref, true);
            std::lock_guard lk(mu_);
            if (!j->peer) j->ref = l["ref"];
            for (auto& f : j->files) {
                auto it = std::find_if(l["files"].begin(), l["files"].end(),
                                       [&](const json& e) { return e["path"] == f.path; });
                if (it == l["files"].end() && j->peer)
                    throw DownloadError(404, "not_found", std::format("{} is not in the models folder of {}", f.path, j->peer_name));
                if (it == l["files"].end())
                    throw DownloadError(404, "not_found", std::format("{} is not in {}@{}", f.path, j->repo, j->ref));
                if ((*it)["size_bytes"].is_number()) f.size = (*it)["size_bytes"].get<std::uint64_t>();
            }
            j->listed = true;
            if (j->stop.empty()) j->state = "downloading";
        }
        // Enough disk space for what is left?
        std::uint64_t need = 0;
        {
            std::lock_guard lk(mu_);
            for (auto& f : j->files) {
                std::error_code ec;
                if (f.state == "done" || f.state == "present") continue;
                if (fs::exists(f.target, ec)) continue;
                std::uint64_t have = file_size_or_zero(part_of(f.target));
                f.done = have;
                if (f.size && *f.size > have) need += *f.size - have;
            }
        }
        if (need > 0) {
            std::error_code ec;
            fs::create_directories(p.models_dir, ec);
            auto sp = fs::space(p.models_dir, ec);
            if (!ec && sp.available < need + 256ull * 1000 * 1000)
                throw DownloadError(507, "disk", std::format("not enough disk space: {} needed, {} free",
                                                             hub::format_bytes(static_cast<double>(need)),
                                                             hub::format_bytes(static_cast<double>(sp.available))));
        }
        for (size_t i = 0; i < j->files.size(); ++i) {
            {
                std::lock_guard lk(mu_);
                if (j->files[i].state == "done" || j->files[i].state == "present") continue;
                j->current = i;
            }
            if (!download_file(j, i, p)) {
                std::lock_guard lk(mu_);
                if (!j->stop.empty()) stopped = true;
                else fail = j->error.empty() ? "download failed" : j->error;
                break;
            }
            if (on_file_done_) on_file_done_();
        }
        if (stop_requested(j)) stopped = true;
    } catch (const DownloadError& e) {
        if (stop_requested(j)) stopped = true;
        else fail = e.what();
    } catch (const std::exception& e) {
        fail = e.what();
    }

    std::string stop;
    {
        std::lock_guard lk(mu_);
        stop = j->stop;
    }
    if (stopped && stop == "cancel") {
        std::error_code ec;
        std::lock_guard lk(mu_);
        for (auto& f : j->files)
            if (f.state != "done" && f.state != "present") {
                fs::remove(part_of(f.target), ec);
                f.done = 0;
                f.state = "pending";
            }
        j->state = "canceled";
        j->speed_bps = 0;
        j->finished_at = j->updated_at = xgi::wall_now();
    } else if (stopped) {
        std::lock_guard lk(mu_);
        for (auto& f : j->files)
            if (f.state == "downloading") f.state = "pending";
        j->state = "paused";
        j->speed_bps = 0;
        j->updated_at = xgi::wall_now();
        xgi::log::info("download {}: paused (partial files kept)", j->id);
    } else if (!fail.empty()) {
        {
            std::lock_guard lk(mu_);
            for (auto& f : j->files)
                if (f.state == "downloading") f.state = "pending";
        }
        finish(j, "failed", fail);
        xgi::log::warn("download {}: failed: {}", j->id, fail);
    } else {
        finish(j, "completed", "");
        xgi::log::info("download {}: completed", j->id);
    }
    std::lock_guard lk(mu_);
    j->running = false;
    j->note.clear();
    j->proc.reset();
}

bool Downloads::download_file(const std::shared_ptr<Job>& j, size_t index, const Paths& paths) {
    fs::path target, part;
    std::optional<std::uint64_t> size;
    std::string url, rel;
    {
        std::lock_guard lk(mu_);
        File& f = j->files[index];
        target = f.target;
        size = f.size;
        rel = f.target_rel;
        url = j->peer ? hub::peer_file_url(j->peer_url, f.path) : hub::file_url(bases_, j->src, j->repo, j->ref, f.path);
    }
    part = part_of(target);
    auto set_file = [&](const std::function<void(File&)>& fn) {
        std::lock_guard lk(mu_);
        fn(j->files[index]);
        j->updated_at = xgi::wall_now();
    };
    auto fail = [&](const std::string& msg) {
        std::lock_guard lk(mu_);
        j->error = msg;
        return false;
    };

    std::error_code ec;
    if (fs::exists(target, ec)) {
        // Never overwrite a model that is already there (it may be loaded by llama-server).
        std::uint64_t have = file_size_or_zero(target);
        if (size && have != *size)
            return fail(std::format("{} already exists with a different size ({} instead of {}); remove it first", rel,
                                    hub::format_bytes(static_cast<double>(have)), hub::format_bytes(static_cast<double>(*size))));
        set_file([&](File& f) {
            f.state = "present";
            f.done = have;
        });
        fs::remove(part, ec);
        xgi::log::info("download {}: {} is already present", j->id, rel);
        return true;
    }
    fs::create_directories(target.parent_path(), ec);
    if (ec) return fail("cannot create " + xgi::path_to_utf8(target.parent_path()) + ": " + ec.message());

    const fs::path work = paths.log_dir / "downloads";
    const fs::path log_file = paths.log_dir / "downloads.log";
    int failures = 0;
    std::uint64_t progress_mark = file_size_or_zero(part);
    set_file([&](File& f) {
        f.state = "downloading";
        f.done = progress_mark;
    });
    xgi::log::info("download {}: {} -> {}", j->id, url, xgi::path_to_utf8(target));

    while (true) {
        if (stop_requested(j)) return false;
        std::uint64_t have = file_size_or_zero(part);
        if (size && have > *size) {  // not ours, or the upstream file changed
            fs::remove(part, ec);
            have = 0;
        }
        if (size && have == *size && have > 0) break;  // complete already (e.g. paused right at the end)

        std::string hdr;
        auto argv = base_args(curl_, !j->peer);
        auto auth = auth_args(*j, work, hdr);
        argv.insert(argv.end(), auth.begin(), auth.end());
        // Abort a stalled transfer (under 1 byte/s for 2 minutes) and resume it below.
        argv.insert(argv.end(), {"--fail", "--speed-limit", "1", "--speed-time", "120", "-C", "-", "-o",
                                 xgi::path_to_utf8(part), "-w", "%{http_code}\\n", url});

        double last_t = xgi::mono_now();
        std::uint64_t last_b = have;
        auto tick = [&]() {
            std::uint64_t now_b = file_size_or_zero(part);
            double now_t = xgi::mono_now();
            std::lock_guard lk(mu_);
            File& f = j->files[index];
            f.done = now_b;
            double dt = now_t - last_t;
            if (dt >= 0.2 && now_b >= last_b) {
                double inst = static_cast<double>(now_b - last_b) / dt;
                j->speed_bps = j->speed_bps <= 0 ? inst : 0.7 * j->speed_bps + 0.3 * inst;
                last_t = now_t;
                last_b = now_b;
            }
            j->updated_at = xgi::wall_now();
            return j->stop.empty();
        };
        auto publish = [&](std::shared_ptr<ManagedProcess> pr) {
            std::lock_guard lk(mu_);
            j->proc = std::move(pr);
        };
        CurlRun r;
        try {
            r = run_curl(argv, log_file, 0, publish, tick);
        } catch (const std::exception& e) {
            if (!hdr.empty()) fs::remove(xgi::path_from_utf8(hdr), ec);
            return fail(std::string("cannot start curl: ") + e.what());
        }
        if (!hdr.empty()) fs::remove(xgi::path_from_utf8(hdr), ec);
        if (stop_requested(j)) return false;

        std::uint64_t now = file_size_or_zero(part);
        CurlOutcome o = parse_curl_output(r.lines);
        if (r.exit_code && *r.exit_code == 0) {
            if (!size || now == *size) break;
            o.error = std::format("size mismatch: got {} bytes, expected {}", now, *size);
            fs::remove(part, ec);  // start over rather than keep a corrupt file
        } else if ((r.exit_code && *r.exit_code == 33) || o.http_status == 416) {
            fs::remove(part, ec);  // server can't resume this file: start it over
        } else if (j->peer && (o.http_status == 429 || o.http_status == 503)) {
            // The peer is already sending its maximum number of files: wait for a free slot. This is
            // not a failure, so it doesn't use up the retries.
            {
                std::lock_guard lk(mu_);
                j->note = std::format("waiting: {} is busy sending other copies", j->peer_name);
                j->speed_bps = 0;
            }
            xgi::log::info("download {}: {} is busy (HTTP {}); waiting {} s", j->id, j->peer_name, o.http_status, kBusyWaitS);
            const double until = xgi::mono_now() + kBusyWaitS;
            while (xgi::mono_now() < until)
                if (stop_requested(j)) return false;
                else std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard lk(mu_);
            j->note.clear();
            continue;
        } else if (j->peer && (o.http_status == 401 || o.http_status == 403)) {
            return fail(std::format("the peer {} rejected the cluster token (HTTP {}): both need the same token",
                                    j->peer_name, o.http_status));
        } else if (j->peer && o.http_status == 404) {
            return fail(std::format("{} is no longer in the models folder of {} (HTTP 404)", rel, j->peer_name));
        } else if (o.http_status == 401 || o.http_status == 403) {
            return fail(std::format("access denied (HTTP {}) for {}: a gated or private file needs {} in the agent's "
                                    "environment",
                                    o.http_status, url, j->src == hub::Source::huggingface ? "HF_TOKEN" : "GITHUB_TOKEN"));
        } else if (o.http_status == 404) {
            return fail("not found (HTTP 404): " + url);
        }
        if (o.error.empty())
            o.error = std::format("curl exit {}", r.exit_code ? std::to_string(*r.exit_code) : std::string("?"));
        if (now > progress_mark) failures = 0;  // it made progress: a fresh set of retries
        progress_mark = now;
        if (++failures > kMaxRetries) return fail(std::format("{}: {} (gave up after {} tries)", rel, o.error, kMaxRetries));
        xgi::log::warn("download {}: {}: {}; retry {}/{} in {} s", j->id, rel, o.error, failures, kMaxRetries, 3 * failures);
        {
            std::lock_guard lk(mu_);
            j->speed_bps = 0;
        }
        const double until = xgi::mono_now() + 3.0 * failures;
        while (xgi::mono_now() < until)
            if (stop_requested(j)) return false;
            else std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (fs::exists(target, ec)) {  // appeared while we downloaded: keep the existing file
        fs::remove(part, ec);
    } else {
        fs::rename(part, target, ec);
        if (ec) return fail("cannot rename " + xgi::path_to_utf8(part) + ": " + ec.message());
    }
    std::uint64_t final_size = file_size_or_zero(target);
    set_file([&](File& f) {
        f.state = "done";
        f.done = final_size;
        if (!f.size) f.size = final_size;
    });
    xgi::log::info("download {}: saved {} ({})", j->id, rel, hub::format_bytes(static_cast<double>(final_size)));
    return true;
}

}  // namespace xi
