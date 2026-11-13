#include <chrono>
#include <future>
#include <thread>

#include "common/util.hpp"
#include "testing.hpp"
#include "xgi/router.hpp"
#include "xgi/scheduling.hpp"

using namespace xgi;
using json = nlohmann::json;

TEST_CASE("scheduling: least busy by inflight/slots") {
    std::vector<GroupLoad> g{{"a", 1, 2, 0, 0, true}, {"b", 1, 4, 0, 0, true}, {"c", 0, 1, 0, 0, false}};
    auto i = select_group(g, 100);
    REQUIRE(i.has_value());
    CHECK_EQ(g[*i].group_id, std::string("b"));
}

TEST_CASE("scheduling: ties go to the group idle longest") {
    std::vector<GroupLoad> g{{"a", 0, 1, 50, 0, true}, {"b", 0, 1, 10, 0, true}};
    CHECK_EQ(g[*select_group(g, 100)].group_id, std::string("b"));
}

TEST_CASE("scheduling: full, suspect and excluded groups are skipped") {
    std::vector<GroupLoad> g{{"a", 1, 1, 0, 0, true}, {"b", 0, 1, 0, 200, true}, {"c", 0, 1, 0, 0, true}};
    CHECK_EQ(g[*select_group(g, 100)].group_id, std::string("c"));
    CHECK(!select_group(g, 100, "c").has_value());
    CHECK(any_routable(g, 100, "c"));  // "a" is busy but routable
    CHECK(!any_routable({{"x", 0, 1, 0, 0, false}}, 100));
}

namespace {
ClusterConfig router_config(int max_queue, double queue_timeout) {
    auto c = parse_config(R"(
pools:
  - {name: p, model_file: p.gguf, nodes_per_group: 1, groups: 1}
  - {name: empty, model_file: e.gguf, nodes_per_group: 1, groups: 0}
)",
                          std::nullopt);
    c.router.max_queue = max_queue;
    c.router.queue_timeout_s = queue_timeout;
    return c;
}
json one_group(const std::string& status) {
    return json::array({json{{"group_id", "p-0"}, {"pool", "p"}, {"status", status}, {"head_url", "http://127.0.0.1:1"}, {"slots", 1}}});
}
}  // namespace

TEST_CASE("router: admission outcomes") {
    Router r(router_config(1, 0.3));
    CHECK(r.acquire("nope").status == Router::Admit::unknown_model);
    CHECK(r.acquire("empty").status == Router::Admit::no_healthy);
    r.update_table(one_group("loading"));
    CHECK(r.acquire("p").status == Router::Admit::no_healthy);
    r.update_table(one_group("healthy"));
    auto a = r.acquire("p");
    REQUIRE(a.status == Router::Admit::ok);
    CHECK_EQ(a.group_id, std::string("p-0"));

    // Slot taken: the next request queues and times out; with the queue full, a third gets 429.
    auto waiter = std::async(std::launch::async, [&] { return r.acquire("p").status; });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(r.acquire("p").status == Router::Admit::queue_full);
    CHECK(waiter.get() == Router::Admit::timeout);
    r.release("p", "p-0");
}

TEST_CASE("router: queued request gets the freed slot") {
    Router r(router_config(10, 5));
    r.update_table(one_group("healthy"));
    REQUIRE(r.acquire("p").status == Router::Admit::ok);
    auto waiter = std::async(std::launch::async, [&] { return r.acquire("p"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    double t0 = mono_now();
    r.release("p", "p-0");
    auto a = waiter.get();
    CHECK(a.status == Router::Admit::ok);
    CHECK(mono_now() - t0 < 1.0);
    CHECK(a.queue_ms >= 90);
    r.release("p", "p-0");
}

TEST_CASE("router: inflight counts survive table refreshes; suspect groups are skipped") {
    Router r(router_config(0, 1));
    r.update_table(one_group("healthy"));
    REQUIRE(r.acquire("p").status == Router::Admit::ok);
    r.update_table(one_group("healthy"));  // refresh must not reset inflight
    CHECK(r.acquire("p").status == Router::Admit::queue_full);
    r.release("p", "p-0");
    r.mark_suspect("p", "p-0");
    CHECK(r.acquire("p").status == Router::Admit::no_healthy);
}
