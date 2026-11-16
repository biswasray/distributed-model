#include <fstream>
#include <random>
#include <sstream>

#include "common/config.hpp"
#include "testing.hpp"
#include "xgi/config_store.hpp"

using namespace xgi;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const char* kYaml = R"(
cluster_token: "file-token"
coordinator: {port: 8100, heartbeat_interval_s: 2.5}
router: {port: 8000, api_keys: [k1, "key with space"]}
llama_cpp: {rpc_cache: false, extra_head_args: ["--metrics"]}
console: {allow_exec: true, model: b, max_tokens: 256}
pools:
  - {name: a, model_file: sub/a.gguf, nodes_per_group: 3, groups: 2, context: 2048, parallel: 2, tensor_split: [1, 1.5, 1], extra_args: ["-fa"]}
  - {name: b, model_file: b.gguf, nodes_per_group: 1, groups: 4}
spares: 1
pinned:
  - {host: PC-01, role: head, pool: a}
)";

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("xgi-cfg-test-" + std::to_string(std::random_device{}()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

TEST_CASE("config: to_yaml round-trips every setting") {
    ClusterConfig c = parse_config(kYaml, std::nullopt);
    ClusterConfig back = parse_config(to_yaml(c), std::nullopt);
    CHECK_EQ(back.cluster_token, c.cluster_token);
    CHECK(back.coordinator == c.coordinator);
    CHECK(back.router == c.router);
    CHECK(back.llama_cpp == c.llama_cpp);
    CHECK(back.console == c.console);
    CHECK(back.pools == c.pools);
    CHECK(back.pinned == c.pinned);
    CHECK_EQ(back.spares, c.spares);
    CHECK(back.warnings.empty());
    // Defaults (no pools, empty lists) survive too.
    ClusterConfig d = default_config(std::nullopt);
    ClusterConfig d2 = parse_config(to_yaml(d), std::nullopt);
    CHECK(d2.pools.empty() && d2.coordinator == d.coordinator && d2.console == d.console);
}

TEST_CASE("config: to_json leaves the token out and parses back as YAML") {
    ClusterConfig c = parse_config(kYaml, std::nullopt);
    json j = to_json(c);
    CHECK(!j.contains("cluster_token"));
    CHECK_EQ(j["pools"][0]["parallel"].get<int>(), 2);
    CHECK(j["pools"][1]["parallel"].is_null());
    ClusterConfig back = parse_config(j.dump(), std::nullopt);  // JSON is YAML
    CHECK(back.pools == c.pools);
    CHECK(back.console == c.console);
}

TEST_CASE("config: default_config takes the env token") {
    CHECK_EQ(default_config(std::string("env")).cluster_token, std::string("env"));
    CHECK(default_config(std::nullopt).pools.empty());
}

TEST_CASE("config store: locate_config search order") {
    TempDir exe_dir;
    auto explicit_loc = locate_config(std::string("custom.yaml"), exe_dir.path);
    CHECK(explicit_loc.found && explicit_loc.path == fs::path("custom.yaml"));
    // No ./cluster.yaml in the test's working dir is assumed; next to the exe is used when present.
    if (!fs::exists("cluster.yaml")) {
        auto none = locate_config(std::nullopt, exe_dir.path);
        CHECK(!none.found && none.path == exe_dir.path / "cluster.yaml");
        std::ofstream(exe_dir.path / "cluster.yaml") << "spares: 0\n";
        auto beside = locate_config(std::nullopt, exe_dir.path);
        CHECK(beside.found && beside.path == exe_dir.path / "cluster.yaml");
    }
}

TEST_CASE("config store: live vs restart changes") {
    ClusterConfig a = parse_config(kYaml, std::nullopt);
    ClusterConfig b = a;
    b.spares = 3;
    b.console.max_tokens = 999;
    b.console.enabled = false;
    b.router.port = 9000;
    b.cluster_token = "new";
    auto live = live_changes(a, b);
    auto restart = restart_changes(a, b);
    CHECK(has(live, "spares") && has(live, "console.max_tokens"));
    CHECK(!has(live, "console.enabled") && !has(live, "router.port"));
    CHECK(has(restart, "router.port") && has(restart, "cluster_token") && has(restart, "console.enabled"));
    ClusterConfig m = merge_live(a, b);
    CHECK_EQ(m.spares, 3);
    CHECK_EQ(m.console.max_tokens, 999);
    CHECK(m.console.enabled);             // restart-only: unchanged
    CHECK_EQ(m.router.port, 8000);        // restart-only: unchanged
    CHECK_EQ(m.cluster_token, std::string("file-token"));
    CHECK(live_changes(a, a).empty() && restart_changes(a, a).empty());
}

TEST_CASE("config store: save writes the file, keeps a backup and applies live parts") {
    TempDir dir;
    fs::path path = dir.path / "cluster.yaml";
    {
        std::ofstream(path, std::ios::binary) << kYaml;
    }
    ClusterConfig running = parse_config(kYaml, std::nullopt);
    ConfigStore store(running, {path, true}, std::nullopt);
    int notified = 0;
    store.on_change([&](const ClusterConfig& c) { notified = c.spares; });

    CHECK(!store.check_yaml("pools: [{name: x}]").ok);  // validation errors come back, nothing written
    CHECK(!store.check_yaml("pools: [unclosed").ok);

    // Form path: JSON in, YAML out, and the file's token is kept even though the form never had it.
    json form = to_json(running);
    form["spares"] = 2;
    form["router"]["port"] = 9000;
    ConfigCheck c = store.check_json(form);
    REQUIRE(c.ok);
    CHECK(has(c.live, "spares") && has(c.restart, "router.port"));
    c = store.save(std::move(c));
    REQUIRE(c.ok);
    CHECK_EQ(notified, 2);
    CHECK_EQ(store.current()->spares, 2);
    CHECK_EQ(store.current()->router.port, 8000);  // needs a restart
    ClusterConfig on_disk = parse_config(read(path), std::nullopt);
    CHECK_EQ(on_disk.cluster_token, std::string("file-token"));
    CHECK_EQ(on_disk.router.port, 9000);
    CHECK(fs::exists(dir.path / "cluster.yaml.bak"));
    json d = store.describe();
    CHECK(d["file_exists"].get<bool>());
    CHECK(d["restart_pending"].size() == 1 && d["restart_pending"][0] == "router.port");

    // YAML path: written byte-for-byte, comments included.
    std::string text = "# my comment\nspares: 0\npools: []\n";
    c = store.save(store.check_yaml(text));
    REQUIRE(c.ok);
    CHECK_EQ(read(path), text);
    CHECK_EQ(store.current()->spares, 0);
}

TEST_CASE("config store: no file yet -> defaults, and saving creates it") {
    TempDir dir;
    fs::path path = dir.path / "sub" / "cluster.yaml";
    ConfigStore store(default_config(std::string("env-token")), {path, false}, std::string("env-token"));
    json d = store.describe();
    CHECK(!d["file_exists"].get<bool>());
    CHECK(d["token_from_env"].get<bool>());
    CHECK(d["yaml"].get<std::string>().find("env-token") == std::string::npos);  // never write the env token
    json form = d["config"];
    form["pools"] = json::array({{{"name", "p"}, {"model_file", "p.gguf"}, {"nodes_per_group", 1}, {"groups", 1}}});
    ConfigCheck c = store.save(store.check_json(form));
    REQUIRE(c.ok);
    CHECK(fs::exists(path));
    CHECK(read(path).find("env-token") == std::string::npos);
    CHECK_EQ(store.current()->pools.size(), size_t{1});
    CHECK_EQ(store.current()->cluster_token, std::string("env-token"));
}
