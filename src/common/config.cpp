#include "common/config.hpp"

#include <yaml-cpp/yaml.h>

#include <format>
#include <fstream>
#include <set>
#include <sstream>

#include "common/util.hpp"

namespace xgi {
namespace {

template <class T>
T get(const YAML::Node& n, const char* key, const T& def, const std::string& where) {
    YAML::Node v = n[key];
    if (!v || v.IsNull()) return def;
    try {
        return v.as<T>();
    } catch (const YAML::Exception&) {
        throw ConfigError(std::format("{}.{}: invalid value", where, key));
    }
}

std::vector<std::string> get_strings(const YAML::Node& n, const char* key, const std::string& where) {
    YAML::Node v = n[key];
    if (!v || v.IsNull()) return {};
    if (!v.IsSequence()) throw ConfigError(std::format("{}.{}: must be a list", where, key));
    std::vector<std::string> out;
    for (const auto& item : v) out.push_back(item.as<std::string>());
    return out;
}

void warn_unknown(const YAML::Node& n, const std::set<std::string>& known, const std::string& where,
                  std::vector<std::string>& warnings) {
    if (!n || !n.IsMap()) return;
    for (const auto& kv : n) {
        auto k = kv.first.as<std::string>();
        if (!known.contains(k)) warnings.push_back(std::format("{}: unknown key '{}' ignored", where, k));
    }
}

}  // namespace

const PoolConfig* ClusterConfig::find_pool(const std::string& name) const {
    for (const auto& p : pools)
        if (p.name == name) return &p;
    return nullptr;
}

int ClusterConfig::total_nodes_requested() const {
    int total = 0;
    for (const auto& p : pools) total += p.nodes_per_group * p.groups;
    return total;
}

std::string normalize_model_path(std::string p) {
    for (auto& c : p)
        if (c == '\\') c = '/';
    while (p.rfind("./", 0) == 0) p = p.substr(2);
    return p;
}

bool is_valid_model_file(const std::string& raw) {
    std::string p = normalize_model_path(raw);
    if (p.empty() || p[0] == '/') return false;
    if (p.size() >= 2 && p[1] == ':') return false;  // drive letter
    for (const auto& part : split(p, '/'))
        if (part == ".." || part.empty()) return false;
    return ends_with_ci(p, ".gguf");
}

ClusterConfig parse_config(const std::string& yaml_text, const std::optional<std::string>& env_token) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        throw ConfigError(std::string("cluster.yaml: YAML syntax error: ") + e.what());
    }
    if (!root || !root.IsMap()) throw ConfigError("cluster.yaml: top level must be a mapping");

    ClusterConfig c;
    warn_unknown(root, {"cluster_token", "coordinator", "router", "llama_cpp", "pools", "spares", "pinned"},
                 "cluster.yaml", c.warnings);

    c.cluster_token = get<std::string>(root, "cluster_token", "", "cluster.yaml");
    if (env_token && !env_token->empty()) c.cluster_token = *env_token;

    if (auto n = root["coordinator"]; n && n.IsMap()) {
        warn_unknown(n, {"host", "port", "heartbeat_interval_s", "node_timeout_s", "group_start_timeout_s"},
                     "coordinator", c.warnings);
        auto& k = c.coordinator;
        k.host = get(n, "host", k.host, "coordinator");
        k.port = get(n, "port", k.port, "coordinator");
        k.heartbeat_interval_s = get(n, "heartbeat_interval_s", k.heartbeat_interval_s, "coordinator");
        k.node_timeout_s = get(n, "node_timeout_s", k.node_timeout_s, "coordinator");
        k.group_start_timeout_s = get(n, "group_start_timeout_s", k.group_start_timeout_s, "coordinator");
    }
    if (auto n = root["router"]; n && n.IsMap()) {
        warn_unknown(n, {"host", "port", "coordinator_url", "max_queue", "queue_timeout_s", "request_timeout_s",
                         "slots_per_group", "api_keys"},
                     "router", c.warnings);
        auto& r = c.router;
        r.host = get(n, "host", r.host, "router");
        r.port = get(n, "port", r.port, "router");
        r.coordinator_url = get(n, "coordinator_url", r.coordinator_url, "router");
        r.max_queue = get(n, "max_queue", r.max_queue, "router");
        r.queue_timeout_s = get(n, "queue_timeout_s", r.queue_timeout_s, "router");
        r.request_timeout_s = get(n, "request_timeout_s", r.request_timeout_s, "router");
        r.slots_per_group = get(n, "slots_per_group", r.slots_per_group, "router");
        r.api_keys = get_strings(n, "api_keys", "router");
    }
    if (auto n = root["llama_cpp"]; n && n.IsMap()) {
        warn_unknown(n, {"rpc_cache", "extra_head_args"}, "llama_cpp", c.warnings);
        c.llama_cpp.rpc_cache = get(n, "rpc_cache", c.llama_cpp.rpc_cache, "llama_cpp");
        c.llama_cpp.extra_head_args = get_strings(n, "extra_head_args", "llama_cpp");
    }

    c.spares = get(root, "spares", 0, "cluster.yaml");
    if (c.spares < 0) throw ConfigError("spares must be >= 0");

    std::set<std::string> names;
    if (auto pools = root["pools"]; pools && !pools.IsNull()) {
        if (!pools.IsSequence()) throw ConfigError("pools: must be a list");
        int idx = 0;
        for (const auto& n : pools) {
            std::string where = std::format("pools[{}]", idx++);
            if (!n.IsMap()) throw ConfigError(where + ": must be a mapping");
            warn_unknown(n, {"name", "model_file", "nodes_per_group", "groups", "context", "parallel",
                             "tensor_split", "extra_args"},
                         where, c.warnings);
            for (const char* req : {"name", "model_file", "nodes_per_group", "groups"})
                if (!n[req] || n[req].IsNull()) throw ConfigError(std::format("{}: '{}' is required", where, req));
            PoolConfig p;
            p.name = get<std::string>(n, "name", "", where);
            where = std::format("pool '{}'", p.name);
            if (p.name.empty()) throw ConfigError(where + ": name must not be empty");
            if (!names.insert(p.name).second) throw ConfigError(std::format("pool name '{}' is not unique", p.name));
            p.model_file = normalize_model_path(get<std::string>(n, "model_file", "", where));
            if (!is_valid_model_file(p.model_file))
                throw ConfigError(std::format("{}: model_file '{}' must be a relative path ending in .gguf", where,
                                              p.model_file));
            p.nodes_per_group = get(n, "nodes_per_group", 1, where);
            p.groups = get(n, "groups", 0, where);
            if (p.nodes_per_group < 1) throw ConfigError(where + ": nodes_per_group must be >= 1");
            if (p.groups < 0) throw ConfigError(where + ": groups must be >= 0");
            p.context = get(n, "context", p.context, where);
            if (p.context < 1) throw ConfigError(where + ": context must be >= 1");
            if (n["parallel"] && !n["parallel"].IsNull()) {
                p.parallel = get(n, "parallel", 1, where);
                if (*p.parallel < 1) throw ConfigError(where + ": parallel must be >= 1");
            }
            if (auto ts = n["tensor_split"]; ts && !ts.IsNull()) {
                if (!ts.IsSequence()) throw ConfigError(where + ": tensor_split must be a list of numbers");
                std::vector<double> v;
                for (const auto& x : ts) v.push_back(x.as<double>());
                if (static_cast<int>(v.size()) != p.nodes_per_group)
                    c.warnings.push_back(std::format("{}: tensor_split has {} entries but nodes_per_group is {}",
                                                     where, v.size(), p.nodes_per_group));
                p.tensor_split = std::move(v);
            }
            p.extra_args = get_strings(n, "extra_args", where);
            c.pools.push_back(std::move(p));
        }
    }

    if (auto pinned = root["pinned"]; pinned && !pinned.IsNull()) {
        if (!pinned.IsSequence()) throw ConfigError("pinned: must be a list");
        for (const auto& n : pinned) {
            PinnedEntry e;
            e.host = get<std::string>(n, "host", "", "pinned");
            e.role = get<std::string>(n, "role", "head", "pinned");
            e.pool = get<std::string>(n, "pool", "", "pinned");
            if (e.host.empty()) throw ConfigError("pinned: 'host' is required");
            if (e.role != "head")
                c.warnings.push_back(std::format("pinned host '{}': only role 'head' is supported", e.host));
            if (!names.contains(e.pool))
                c.warnings.push_back(std::format("pinned host '{}': unknown pool '{}'", e.host, e.pool));
            c.pinned.push_back(std::move(e));
        }
    }

    if (c.router.slots_per_group < 1) throw ConfigError("router.slots_per_group must be >= 1");
    if (c.router.max_queue < 0) throw ConfigError("router.max_queue must be >= 0");
    if (c.coordinator.heartbeat_interval_s <= 0) throw ConfigError("coordinator.heartbeat_interval_s must be > 0");
    return c;
}

ClusterConfig load_config(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ConfigError("cannot open config file " + path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_config(ss.str(), get_env(kTokenEnv));
}

}  // namespace xgi
