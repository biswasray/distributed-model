#include <algorithm>
#include <random>

#include "testing.hpp"
#include "xi/settings.hpp"

using namespace xi;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
}  // namespace

TEST_CASE("xi settings: yaml round trip and defaults") {
    AgentSettings d;
    CHECK(parse_settings_yaml("") == d);  // empty file = defaults
    AgentSettings s;
    s.coordinator = "http://192.168.1.10:8100";
    s.name = "LAB-PC-07";
    s.models_dir = "D:\\models with space";
    s.heartbeat_s = 2.5;
    s.allow_exec = true;
    s.port = 18200;
    CHECK(parse_settings_yaml(to_yaml(s)) == s);
    CHECK(to_yaml(s).find("token") == std::string::npos || to_yaml(s).find("token is NOT stored") != std::string::npos);
}

TEST_CASE("xi settings: validation and unknown keys") {
    CHECK_THROWS(parse_settings_yaml("coordinater: http://x:1\n"));  // typo is an error, not ignored
    CHECK_THROWS(parse_settings_yaml("token: secret\n"));           // never in the file
    CHECK_THROWS(parse_settings_yaml("port: [1]\n"));
    CHECK_THROWS(apply_json(AgentSettings{}, json{{"coordinator", "ftp://x"}}));
    CHECK_THROWS(apply_json(AgentSettings{}, json{{"port", 50052}}));  // clashes with rpc_port
    CHECK_THROWS(apply_json(AgentSettings{}, json{{"heartbeat_s", 0}}));
    CHECK_THROWS(apply_json(AgentSettings{}, json{{"port", "8200"}}));  // wrong type
    CHECK_THROWS(apply_json(AgentSettings{}, json{{"name", "a/b"}}));
    CHECK_THROWS(apply_json(AgentSettings{}, json{{"models_dir", ""}}));
    auto s = apply_json(AgentSettings{}, json{{"coordinator", ""}, {"exec_dir", "work"}});
    CHECK(s.coordinator.empty());  // "not configured yet" is allowed
    CHECK_EQ(s.exec_dir, std::string("work"));
}

TEST_CASE("xi settings: live vs restart keys") {
    AgentSettings a;
    AgentSettings b = a;
    b.coordinator = "http://10.0.0.1:8100";
    b.port = 9200;
    b.rpc_port = 50099;
    b.allow_exec = true;
    auto changed = changed_keys(a, b);
    CHECK(has(changed, "coordinator") && has(changed, "port") && has(changed, "allow_exec"));
    AgentSettings m = merge_live(a, b);
    CHECK_EQ(m.coordinator, b.coordinator);
    CHECK(m.allow_exec);
    CHECK_EQ(m.port, 8200);       // restart key: unchanged
    CHECK_EQ(m.rpc_port, 50052);  // restart key: unchanged
    for (const auto& k : restart_keys()) CHECK(has(settings_keys(), k));
    CHECK_EQ(flag_for("exec_max_timeout_s"), std::string("--exec-max-timeout-s"));
}

TEST_CASE("xi settings: loopback detection") {
    CHECK(is_loopback("127.0.0.1"));
    CHECK(is_loopback("127.1.2.3"));
    CHECK(is_loopback("::1"));
    CHECK(is_loopback("::ffff:127.0.0.1"));
    CHECK(!is_loopback("192.168.1.20"));
    CHECK(!is_loopback("::ffff:192.168.1.20"));
    CHECK(!is_loopback("1127.0.0.1"));
    CHECK(!is_loopback(""));
}

TEST_CASE("xi settings: locate xi.yaml") {
    fs::path dir = fs::temp_directory_path() / ("xi-settings-test-" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    auto explicit_missing = locate_settings((dir / "x.yaml").string(), dir);
    CHECK(!explicit_missing.found && explicit_missing.path == dir / "x.yaml");  // created by the first save
    if (!fs::exists("xi.yaml")) {
        auto none = locate_settings(std::nullopt, dir);
        CHECK(!none.found && none.path == dir / "xi.yaml");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
