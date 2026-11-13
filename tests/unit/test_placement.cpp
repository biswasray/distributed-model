#include <algorithm>
#include <format>

#include "testing.hpp"
#include "xgi/placement.hpp"

using namespace xgi;

namespace {

constexpr std::uint64_t GB = 1'000'000'000ull;

PoolConfig pool(const std::string& name, const std::string& file, int npg, int groups = 1) {
    PoolConfig p;
    p.name = name;
    p.model_file = file;
    p.nodes_per_group = npg;
    p.groups = groups;
    return p;
}

FreeNode node(const std::string& id, std::uint64_t ram_gb, std::set<std::string> models = {}) {
    return FreeNode{id, id, ram_gb * GB, std::move(models)};
}

void remove_assigned(std::vector<FreeNode>& free, const Placement& p) {
    std::erase_if(free, [&](const FreeNode& n) {
        return n.node_id == p.head || std::find(p.workers.begin(), p.workers.end(), n.node_id) != p.workers.end();
    });
}

}  // namespace

TEST_CASE("placement: head holds the model, workers ordered by RAM") {
    auto p = pool("moe", "moe.gguf", 3);
    std::vector<FreeNode> free{node("A", 8, {"moe.gguf"}), node("B", 6), node("C", 9), node("D", 7)};
    auto r = place_group(p, free, 0, {}, {"moe.gguf"});
    REQUIRE(r.ok);
    CHECK_EQ(r.head, std::string("A"));
    REQUIRE(r.workers.size() == 2);
    CHECK_EQ(r.workers[0], std::string("C"));
    CHECK_EQ(r.workers[1], std::string("D"));
}

TEST_CASE("placement: head with most free RAM wins, pinned wins over RAM") {
    auto p = pool("moe", "moe.gguf", 1);
    std::vector<FreeNode> free{node("A", 6, {"moe.gguf"}), node("B", 8, {"moe.gguf"})};
    CHECK_EQ(place_group(p, free, 0, {}, {"moe.gguf"}).head, std::string("B"));
    std::vector<PinnedEntry> pinned{{"A", "head", "moe"}};
    CHECK_EQ(place_group(p, free, 0, pinned, {"moe.gguf"}).head, std::string("A"));
    std::vector<PinnedEntry> other_pool{{"A", "head", "other"}};
    CHECK_EQ(place_group(p, free, 0, other_pool, {"moe.gguf"}).head, std::string("B"));
}

TEST_CASE("placement: workers avoid nodes holding any pool's model file") {
    auto p = pool("moe", "moe.gguf", 2);
    std::vector<FreeNode> free{node("H", 8, {"moe.gguf"}), node("M", 9, {"7b.gguf"}), node("W", 6)};
    auto r = place_group(p, free, 0, {}, {"moe.gguf", "7b.gguf"});
    REQUIRE(r.ok);
    CHECK_EQ(r.workers[0], std::string("W"));
    // A file that is not any pool's model_file doesn't count.
    auto r2 = place_group(p, free, 0, {}, {"moe.gguf"});
    CHECK_EQ(r2.workers[0], std::string("M"));
}

TEST_CASE("placement: spares are kept free") {
    auto p = pool("moe", "moe.gguf", 3);
    std::vector<FreeNode> free{node("A", 8, {"moe.gguf"}), node("B", 8), node("C", 8), node("D", 8)};
    CHECK(place_group(p, free, 1, {}, {"moe.gguf"}).ok);
    auto r = place_group(p, free, 2, {}, {"moe.gguf"});
    CHECK(!r.ok);
    CHECK(r.reason.find("spares") != std::string::npos);
    CHECK(place_group(p, free, 0, {}, {"moe.gguf"}).ok);  // repair may use spares
}

TEST_CASE("placement: no model holder or too few nodes is unplaced") {
    auto p = pool("moe", "moe.gguf", 2);
    std::vector<FreeNode> free{node("A", 8), node("B", 8)};
    auto r = place_group(p, free, 0, {}, {"moe.gguf"});
    CHECK(!r.ok);
    CHECK(r.reason.find("model file") != std::string::npos);
    std::vector<FreeNode> one{node("A", 8, {"moe.gguf"})};
    CHECK(!place_group(p, one, 0, {}, {"moe.gguf"}).ok);
}

TEST_CASE("placement: hybrid layout (operator guide option D) fits 50 nodes") {
    // 1x8 72B + 10x3 MoE + 10x1 7B + 2 spares = 50
    std::vector<PoolConfig> pools{pool("qwen3-30b-a3b", "moe.gguf", 3, 10), pool("qwen-72b", "72b.gguf", 8, 1),
                                  pool("qwen-7b", "7b.gguf", 1, 10)};
    std::set<std::string> files{"moe.gguf", "72b.gguf", "7b.gguf"};
    std::vector<FreeNode> free;
    for (int i = 0; i < 50; ++i) {
        std::set<std::string> m;
        if (i < 10) m.insert("moe.gguf");
        else if (i < 11) m.insert("72b.gguf");
        else if (i < 21) m.insert("7b.gguf");
        free.push_back(node(std::format("PC-{:02}", i), 8, m));
    }
    int placed = 0;
    for (const auto& p : pools)
        for (int g = 0; g < p.groups; ++g) {
            auto r = place_group(p, free, 2, {}, files);
            if (!r.ok) continue;
            ++placed;
            remove_assigned(free, r);
        }
    CHECK_EQ(placed, 21);
    CHECK_EQ(free.size(), size_t{2});
}
