#include "xgi/config_store.hpp"

#include <fstream>
#include <sstream>

#include "common/log.hpp"
#include "common/platform.hpp"
#include "common/util.hpp"

namespace xgi {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// Adds "<prefix>.<key>" for every key whose value differs between two JSON objects.
void diff_keys(const json& a, const json& b, const std::string& prefix, std::vector<std::string>& out) {
    for (auto it = b.begin(); it != b.end(); ++it)
        if (!a.contains(it.key()) || a[it.key()] != it.value()) out.push_back(prefix + "." + it.key());
}

}  // namespace

ConfigLocation locate_config(const std::optional<std::string>& explicit_path, const fs::path& exe_dir) {
    if (explicit_path) return {path_from_utf8(*explicit_path), true};
    std::error_code ec;
    fs::path here = fs::absolute("cluster.yaml", ec);
    if (fs::is_regular_file(here, ec)) return {here, true};
    if (!exe_dir.empty()) {
        fs::path beside = exe_dir / "cluster.yaml";
        if (fs::is_regular_file(beside, ec)) return {beside, true};
        return {beside, false};
    }
    return {here, false};
}

std::vector<std::string> live_changes(const ClusterConfig& running, const ClusterConfig& next) {
    std::vector<std::string> out;
    json a = to_json(running), b = to_json(next);
    for (const char* k : {"pools", "spares", "pinned"})
        if (a[k] != b[k]) out.push_back(k);
    diff_keys(a["llama_cpp"], b["llama_cpp"], "llama_cpp", out);
    a["console"].erase("enabled");
    b["console"].erase("enabled");
    diff_keys(a["console"], b["console"], "console", out);
    return out;
}

std::vector<std::string> restart_changes(const ClusterConfig& running, const ClusterConfig& next) {
    std::vector<std::string> out;
    if (running.cluster_token != next.cluster_token) out.push_back("cluster_token");
    json a = to_json(running), b = to_json(next);
    diff_keys(a["coordinator"], b["coordinator"], "coordinator", out);
    diff_keys(a["router"], b["router"], "router", out);
    if (running.console.enabled != next.console.enabled) out.push_back("console.enabled");
    return out;
}

ClusterConfig merge_live(ClusterConfig running, const ClusterConfig& next) {
    running.pools = next.pools;
    running.spares = next.spares;
    running.pinned = next.pinned;
    running.llama_cpp = next.llama_cpp;
    bool enabled = running.console.enabled;
    running.console = next.console;
    running.console.enabled = enabled;
    running.warnings = next.warnings;
    return running;
}

json ConfigCheck::to_json() const {
    json j{{"ok", ok}, {"warnings", warnings}, {"live", live}, {"restart", restart}};
    if (!ok) j["error"] = error;
    else j["yaml"] = yaml;
    return j;
}

// ------------------------------------------------------------------------------------------ store

ConfigStore::ConfigStore(ClusterConfig running, ConfigLocation where, std::optional<std::string> env_token)
    : running_(std::make_shared<const ClusterConfig>(std::move(running))),
      where_(std::move(where)),
      env_token_(std::move(env_token)) {}

std::shared_ptr<const ClusterConfig> ConfigStore::current() const {
    std::lock_guard lk(mu_);
    return running_;
}

void ConfigStore::on_change(std::function<void(const ClusterConfig&)> fn) {
    std::lock_guard lk(mu_);
    listeners_.push_back(std::move(fn));
}

std::optional<std::string> ConfigStore::read_file() const {
    std::ifstream f(where_.path, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

json ConfigStore::describe() const {
    auto running = current();
    auto text = read_file();
    json out{{"path", path_to_utf8(where_.path)},
             {"file_exists", text.has_value()},
             {"running", to_json(*running)},
             {"token_set", !running->cluster_token.empty()},
             {"token_from_env", env_token_ && !env_token_->empty()}};
    if (!text) {
        // No file yet: offer the running (default) settings, without leaking an env token into it.
        ClusterConfig shown = *running;
        shown.cluster_token.clear();
        out["yaml"] = to_yaml(shown);
        out["config"] = to_json(*running);
        out["restart_pending"] = json::array();
        out["parse_error"] = nullptr;
        return out;
    }
    out["yaml"] = *text;
    try {
        ClusterConfig file = parse_config(*text, env_token_);
        out["config"] = to_json(file);
        out["restart_pending"] = restart_changes(*running, file);
        out["parse_error"] = nullptr;
    } catch (const std::exception& e) {
        out["config"] = to_json(*running);
        out["restart_pending"] = json::array();
        out["parse_error"] = e.what();
    }
    return out;
}

ConfigCheck ConfigStore::diff(ConfigCheck c) const {
    auto running = current();
    c.warnings = c.parsed.warnings;
    c.live = live_changes(*running, c.parsed);
    c.restart = restart_changes(*running, c.parsed);
    c.ok = true;
    return c;
}

ConfigCheck ConfigStore::check_yaml(const std::string& yaml) const {
    ConfigCheck c;
    try {
        c.parsed = parse_config(yaml, env_token_);
    } catch (const std::exception& e) {
        c.error = e.what();
        return c;
    }
    c.yaml = yaml;
    return diff(std::move(c));
}

ConfigCheck ConfigStore::check_json(const json& config) const {
    ConfigCheck c;
    if (!config.is_object()) {
        c.error = "config must be a JSON object with cluster.yaml keys";
        return c;
    }
    // The form never sees the token: keep whatever the file has.
    std::string file_token;
    if (auto text = read_file()) {
        try {
            file_token = parse_config(*text, std::nullopt).cluster_token;
        } catch (const std::exception&) {
        }
    }
    json j = config;
    j["cluster_token"] = file_token;
    try {
        // JSON is valid YAML, so the normal parser and its validation rules apply unchanged.
        ClusterConfig as_file = parse_config(j.dump(), std::nullopt);
        c.yaml = to_yaml(as_file);
        c.parsed = parse_config(c.yaml, env_token_);
    } catch (const std::exception& e) {
        c.error = e.what();
        return c;
    }
    return diff(std::move(c));
}

ConfigCheck ConfigStore::save(ConfigCheck c) {
    if (!c.ok) return c;
    const fs::path& path = where_.path;
    if (std::string err = write_file_atomic(path, c.yaml); !err.empty()) {
        c.ok = false;
        c.error = err;
        return c;
    }

    std::shared_ptr<const ClusterConfig> next;
    std::vector<std::function<void(const ClusterConfig&)>> listeners;
    {
        std::lock_guard lk(mu_);
        next = std::make_shared<const ClusterConfig>(merge_live(*running_, c.parsed));
        running_ = next;
        where_.found = true;
        listeners = listeners_;
    }
    log::info("config: saved {} (applied now: [{}]; after restart: [{}])", path_to_utf8(path), join(c.live, ", "),
              join(c.restart, ", "));
    for (auto& fn : listeners) fn(*next);
    return c;
}

}  // namespace xgi
