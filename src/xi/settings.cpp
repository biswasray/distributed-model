#include "xi/settings.hpp"

#include <yaml-cpp/yaml.h>

#include <format>
#include <stdexcept>

#include "common/http_util.hpp"
#include "common/platform.hpp"

namespace xi {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

enum class Kind { str, integer, number, boolean };

struct KeyInfo {
    const char* key;
    Kind kind;
};

// Display order: connection, llama.cpp, code execution, then restart-only.
const KeyInfo kKeys[] = {
    {"coordinator", Kind::str},       {"name", Kind::str},
    {"advertise_ip", Kind::str},      {"heartbeat_s", Kind::number},
    {"llama_dir", Kind::str},         {"rpc_server_bin", Kind::str},
    {"llama_server_bin", Kind::str},  {"models_dir", Kind::str},
    {"allow_exec", Kind::boolean},    {"exec_dir", Kind::str},
    {"exec_max_timeout_s", Kind::number}, {"host", Kind::str},
    {"port", Kind::integer},          {"log_dir", Kind::str},
    {"rpc_port", Kind::integer},      {"head_port", Kind::integer},
};

const KeyInfo* find_key(const std::string& k) {
    for (const auto& i : kKeys)
        if (k == i.key) return &i;
    return nullptr;
}

void check_port(int p, const char* key) {
    if (p < 1 || p > 65535) throw std::invalid_argument(std::format("{} must be 1-65535", key));
}

}  // namespace

const std::set<std::string>& restart_keys() {
    static const std::set<std::string> k{"host", "port", "log_dir", "rpc_port", "head_port"};
    return k;
}

const std::vector<std::string>& settings_keys() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const auto& i : kKeys) out.push_back(i.key);
        return out;
    }();
    return v;
}

std::string flag_for(const std::string& key) {
    std::string f = "--" + key;
    for (auto& c : f)
        if (c == '_') c = '-';
    return f;
}

json to_json(const AgentSettings& s) {
    return json{{"coordinator", s.coordinator},
                {"name", s.name},
                {"advertise_ip", s.advertise_ip},
                {"heartbeat_s", s.heartbeat_s},
                {"llama_dir", s.llama_dir},
                {"rpc_server_bin", s.rpc_server_bin},
                {"llama_server_bin", s.llama_server_bin},
                {"models_dir", s.models_dir},
                {"allow_exec", s.allow_exec},
                {"exec_dir", s.exec_dir},
                {"exec_max_timeout_s", s.exec_max_timeout_s},
                {"host", s.host},
                {"port", s.port},
                {"log_dir", s.log_dir},
                {"rpc_port", s.rpc_port},
                {"head_port", s.head_port}};
}

AgentSettings apply_json(AgentSettings s, const json& j) {
    if (!j.is_object()) throw std::invalid_argument("settings must be a JSON object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        const KeyInfo* k = find_key(it.key());
        if (!k) throw std::invalid_argument("unknown setting '" + it.key() + "'");
        const json& v = it.value();
        auto bad = [&](const char* what) { return std::invalid_argument(std::format("{} must be {}", it.key(), what)); };
        switch (k->kind) {
            case Kind::str:
                if (!v.is_string()) throw bad("a string");
                break;
            case Kind::integer:
                if (!v.is_number_integer()) throw bad("an integer");
                break;
            case Kind::number:
                if (!v.is_number()) throw bad("a number");
                break;
            case Kind::boolean:
                if (!v.is_boolean()) throw bad("true or false");
                break;
        }
        const std::string& key = it.key();
        if (key == "coordinator") s.coordinator = v;
        else if (key == "name") s.name = v;
        else if (key == "advertise_ip") s.advertise_ip = v;
        else if (key == "heartbeat_s") s.heartbeat_s = v;
        else if (key == "llama_dir") s.llama_dir = v;
        else if (key == "rpc_server_bin") s.rpc_server_bin = v;
        else if (key == "llama_server_bin") s.llama_server_bin = v;
        else if (key == "models_dir") s.models_dir = v;
        else if (key == "allow_exec") s.allow_exec = v;
        else if (key == "exec_dir") s.exec_dir = v;
        else if (key == "exec_max_timeout_s") s.exec_max_timeout_s = v;
        else if (key == "host") s.host = v;
        else if (key == "port") s.port = v;
        else if (key == "log_dir") s.log_dir = v;
        else if (key == "rpc_port") s.rpc_port = v;
        else if (key == "head_port") s.head_port = v;
    }
    validate(s);
    return s;
}

void validate(const AgentSettings& s) {
    if (!s.coordinator.empty() && !xgi::parse_base_url(s.coordinator))
        throw std::invalid_argument("coordinator must be an http://<ip>:<port> URL");
    if (s.heartbeat_s <= 0) throw std::invalid_argument("heartbeat_s must be > 0");
    if (s.exec_max_timeout_s <= 0) throw std::invalid_argument("exec_max_timeout_s must be > 0");
    check_port(s.port, "port");
    check_port(s.rpc_port, "rpc_port");
    check_port(s.head_port, "head_port");
    if (s.port == s.rpc_port || s.port == s.head_port || s.rpc_port == s.head_port)
        throw std::invalid_argument("port, rpc_port and head_port must all differ");
    for (const auto* d : {&s.llama_dir, &s.models_dir, &s.log_dir, &s.exec_dir, &s.host})
        if (d->empty()) throw std::invalid_argument("host, llama_dir, models_dir, log_dir and exec_dir must not be empty");
    for (char c : s.name)
        if (c == '/' || c == '\\' || c == '?' || c == '#' || static_cast<unsigned char>(c) < 0x20)
            throw std::invalid_argument("name must not contain / \\ ? # or control characters");
}

AgentSettings parse_settings_yaml(const std::string& text) {
    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const YAML::Exception& e) {
        throw std::invalid_argument(std::string("xi.yaml: YAML syntax error: ") + e.what());
    }
    if (!root || root.IsNull()) return AgentSettings{};
    if (!root.IsMap()) throw std::invalid_argument("xi.yaml: top level must be a mapping");
    json j = json::object();
    for (const auto& kv : root) {
        std::string key = kv.first.as<std::string>();
        if (key == "token" || key == "cluster_token")
            throw std::invalid_argument("xi.yaml: the token is not stored here; use QWEN_CLUSTER_TOKEN or --token");
        const KeyInfo* k = find_key(key);
        if (!k) throw std::invalid_argument("xi.yaml: unknown key '" + key + "'");
        if (kv.second.IsNull()) continue;
        try {
            switch (k->kind) {
                case Kind::str: j[key] = kv.second.as<std::string>(); break;
                case Kind::integer: j[key] = kv.second.as<int>(); break;
                case Kind::number: j[key] = kv.second.as<double>(); break;
                case Kind::boolean: j[key] = kv.second.as<bool>(); break;
            }
        } catch (const YAML::Exception&) {
            throw std::invalid_argument("xi.yaml: invalid value for '" + key + "'");
        }
    }
    try {
        return apply_json(AgentSettings{}, j);
    } catch (const std::invalid_argument& e) {
        throw std::invalid_argument(std::string("xi.yaml: ") + e.what());
    }
}

std::string to_yaml(const AgentSettings& s) {
    YAML::Emitter out;
    out.SetDoublePrecision(15);
    out << YAML::BeginMap;
    json j = to_json(s);
    for (const auto& i : kKeys) {
        out << YAML::Key << i.key << YAML::Value;
        const json& v = j[i.key];
        switch (i.kind) {
            case Kind::str: out << YAML::DoubleQuoted << v.get<std::string>(); break;
            case Kind::integer: out << v.get<int>(); break;
            case Kind::number: out << v.get<double>(); break;
            case Kind::boolean: out << v.get<bool>(); break;
        }
    }
    out << YAML::EndMap;
    return std::string("# XI node agent settings (architecture/02-node-agent.md section 10).\n"
                       "# Command-line flags override these. The cluster token is NOT stored here.\n") +
           out.c_str() + "\n";
}

std::vector<std::string> changed_keys(const AgentSettings& a, const AgentSettings& b) {
    std::vector<std::string> out;
    json ja = to_json(a), jb = to_json(b);
    for (const auto& i : kKeys)
        if (ja[i.key] != jb[i.key]) out.push_back(i.key);
    return out;
}

AgentSettings merge_live(AgentSettings running, const AgentSettings& next) {
    AgentSettings out = next;
    out.host = running.host;
    out.port = running.port;
    out.log_dir = running.log_dir;
    out.rpc_port = running.rpc_port;
    out.head_port = running.head_port;
    return out;
}

bool is_loopback(const std::string& addr) {
    return addr == "::1" || addr.rfind("127.", 0) == 0 || addr.rfind("::ffff:127.", 0) == 0;
}

SettingsLocation locate_settings(const std::optional<std::string>& explicit_path, const fs::path& exe_dir) {
    if (explicit_path) return {xgi::path_from_utf8(*explicit_path), fs::exists(xgi::path_from_utf8(*explicit_path))};
    std::error_code ec;
    fs::path here = fs::absolute("xi.yaml", ec);
    if (fs::is_regular_file(here, ec)) return {here, true};
    if (!exe_dir.empty()) {
        fs::path beside = exe_dir / "xi.yaml";
        return {beside, fs::is_regular_file(beside, ec)};
    }
    return {here, false};
}

}  // namespace xi
