#include "common/config.hpp"
#include "testing.hpp"

using namespace xgi;

namespace {
const char* kGood = R"(
cluster_token: "from-yaml"
coordinator:
  port: 8100
  heartbeat_interval_s: 5
router:
  max_queue: 7
  api_keys: [k1, k2]
llama_cpp:
  rpc_cache: false
  extra_head_args: ["--metrics"]
pools:
  - name: qwen3-30b-a3b
    model_file: Qwen3-30B-A3B-Q4_K_M.gguf
    nodes_per_group: 3
    groups: 10
    context: 4096
    parallel: 2
    tensor_split: [1, 1, 1]
  - name: qwen-7b
    model_file: sub\qwen2.5-7b-instruct-q4_k_m.gguf
    nodes_per_group: 1
    groups: 10
spares: 2
pinned:
  - host: LAB-PC-01
    role: head
    pool: qwen3-30b-a3b
)";
}  // namespace

TEST_CASE("config: parses the documented schema") {
    auto c = parse_config(kGood, std::nullopt);
    CHECK_EQ(c.cluster_token, std::string("from-yaml"));
    CHECK_EQ(c.router.max_queue, 7);
    CHECK_EQ(c.router.api_keys.size(), size_t{2});
    CHECK(!c.llama_cpp.rpc_cache);
    REQUIRE(c.pools.size() == 2);
    CHECK_EQ(c.pools[0].nodes_per_group, 3);
    CHECK_EQ(c.slots_for(c.pools[0]), 2);
    CHECK_EQ(c.slots_for(c.pools[1]), 1);  // router.slots_per_group default
    CHECK_EQ(c.pools[1].model_file, std::string("sub/qwen2.5-7b-instruct-q4_k_m.gguf"));
    CHECK_EQ(c.pools[1].context, 4096);
    CHECK_EQ(c.spares, 2);
    CHECK_EQ(c.total_nodes_requested(), 40);
    CHECK_EQ(c.pinned.size(), size_t{1});
    CHECK(c.warnings.empty());
}

TEST_CASE("config: QWEN_CLUSTER_TOKEN overrides cluster.yaml") {
    auto c = parse_config(kGood, std::string("from-env"));
    CHECK_EQ(c.cluster_token, std::string("from-env"));
    auto d = parse_config(kGood, std::string(""));
    CHECK_EQ(d.cluster_token, std::string("from-yaml"));
}

TEST_CASE("config: defaults") {
    auto c = parse_config("pools: []\n", std::nullopt);
    CHECK_EQ(c.coordinator.port, 8100);
    CHECK_EQ(c.router.port, 8000);
    CHECK_EQ(c.coordinator.node_timeout_s, 30.0);
    CHECK_EQ(c.coordinator.group_start_timeout_s, 7200.0);
    CHECK_EQ(c.router.request_timeout_s, 1800.0);
    CHECK_EQ(c.router.coordinator_url, std::string("http://127.0.0.1:8100"));
    CHECK(c.llama_cpp.rpc_cache);
}

TEST_CASE("config: validation rules fail startup") {
    auto bad = [](const std::string& pool) {
        return [pool] { parse_config("pools:\n" + pool, std::nullopt); };
    };
    CHECK_THROWS(bad("  - {model_file: a.gguf, nodes_per_group: 1, groups: 1}\n")());          // no name
    CHECK_THROWS(bad("  - {name: a, nodes_per_group: 1, groups: 1}\n")());                      // no model_file
    CHECK_THROWS(bad("  - {name: a, model_file: a.gguf, groups: 1}\n")());                      // no nodes_per_group
    CHECK_THROWS(bad("  - {name: a, model_file: a.gguf, nodes_per_group: 0, groups: 1}\n")());
    CHECK_THROWS(bad("  - {name: a, model_file: a.gguf, nodes_per_group: 1, groups: -1}\n")());
    CHECK_THROWS(bad("  - {name: a, model_file: C:\\m\\a.gguf, nodes_per_group: 1, groups: 1}\n")());
    CHECK_THROWS(bad("  - {name: a, model_file: ../a.gguf, nodes_per_group: 1, groups: 1}\n")());
    CHECK_THROWS(bad("  - {name: a, model_file: a.bin, nodes_per_group: 1, groups: 1}\n")());
    CHECK_THROWS(bad("  - {name: a, model_file: a.gguf, nodes_per_group: 1, groups: 1}\n"
                     "  - {name: a, model_file: b.gguf, nodes_per_group: 1, groups: 1}\n")());  // duplicate
    CHECK_THROWS(parse_config("pools: [ {", std::nullopt));                                      // YAML syntax
}

TEST_CASE("config: unknown keys are warnings, not errors") {
    auto c = parse_config("llama_cpp:\n  path: C:\\x\n  threads: auto\npools: []\nfoo: 1\n", std::nullopt);
    CHECK_EQ(c.warnings.size(), size_t{3});
}

TEST_CASE("config: model file validation helper") {
    CHECK(is_valid_model_file("Qwen3-30B-A3B-Q4_K_M.gguf"));
    CHECK(is_valid_model_file("dir/x-00001-of-00012.GGUF"));
    CHECK(is_valid_model_file(".\\dir\\x.gguf"));
    CHECK(!is_valid_model_file("/abs/x.gguf"));
    CHECK(!is_valid_model_file("C:/x.gguf"));
    CHECK(!is_valid_model_file("a/../../x.gguf"));
    CHECK(!is_valid_model_file("x.gguf.txt"));
    CHECK(!is_valid_model_file(""));
}
