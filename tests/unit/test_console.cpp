#include "common/config.hpp"
#include "testing.hpp"
#include "xgi/console.hpp"
#include "xgi/ui_assets.hpp"

using namespace xgi;
using json = nlohmann::json;

TEST_CASE("console: extract_code takes the first fenced block") {
    CHECK_EQ(extract_code("Here you go:\n```python\nprint('hi')\nprint(2)\n```\nDone."), std::string("print('hi')\nprint(2)"));
    CHECK_EQ(extract_code("```\nx = 1\n```"), std::string("x = 1"));
    CHECK_EQ(extract_code("  just code\n"), std::string("just code"));  // no fence: whole reply
    // An unterminated block (max_tokens reached) keeps everything after the fence line.
    CHECK_EQ(extract_code("```js\nconsole.log(1)\nconsole.lo"), std::string("console.log(1)\nconsole.lo"));
    // Triple backticks inside a line are not the closing fence.
    CHECK_EQ(extract_code("```sh\necho '```'\n```"), std::string("echo '```'"));
}

TEST_CASE("console: extract_code drops <think> blocks") {
    CHECK_EQ(extract_code("<think>use a loop ```no```</think>\n```py\nfor i in range(3): print(i)\n```"),
             std::string("for i in range(3): print(i)"));
    CHECK_EQ(extract_code("<think>still thinking"), std::string(""));
}

TEST_CASE("console: generation prompt names the node's OS and runtime") {
    json node{{"os", "Windows-11-10.0.26200-SP0"},
              {"arch", "x86_64"},
              {"runtimes", {{{"language", "python"}, {"label", "Python"}, {"version", "Python 3.12.1"}}}}};
    json history = json::array({{{"role", "user"}, {"content", "first"}},
                                {{"role", "assistant"}, {"content", "```python\nprint(1)\n```"}},
                                {{"role", "system"}, {"content", "ignored: only user/assistant turns pass"}},
                                {{"role", "user"}, {"content", 42}}});
    json m = build_generate_messages("python", node, "list files", history);
    REQUIRE(m.is_array() && m.size() == 4);
    CHECK_EQ(m[0]["role"].get<std::string>(), std::string("system"));
    std::string sys = m[0]["content"];
    CHECK(sys.find("Windows-11-10.0.26200-SP0 (x86_64)") != std::string::npos);
    CHECK(sys.find("Python (Python 3.12.1)") != std::string::npos);
    CHECK_EQ(m[1]["content"].get<std::string>(), std::string("first"));
    CHECK_EQ(m[2]["role"].get<std::string>(), std::string("assistant"));
    CHECK_EQ(m[3]["content"].get<std::string>(), std::string("list files"));
    // No node: still a valid conversation.
    CHECK_EQ(build_generate_messages("bash", nullptr, "x", nullptr).size(), size_t{2});
}

TEST_CASE("console: public paths and static files") {
    CHECK(Console::is_public_path("/"));
    CHECK(Console::is_public_path("/ui"));
    CHECK(Console::is_public_path("/ui/app.js"));
    CHECK(!Console::is_public_path("/nodes"));
    CHECK(!Console::is_public_path("/console/exec"));
    CHECK(!Console::is_public_path("/uix"));
    for (const char* f : {"index.html", "app.css", "app.js"}) {
        const ui::Asset* a = ui::find_asset(f);
        REQUIRE(a != nullptr);
        CHECK(a->size > 100);
    }
    CHECK(ui::find_asset("../cluster.yaml") == nullptr);
    CHECK_EQ(mime_type("index.html"), std::string("text/html; charset=utf-8"));
    CHECK_EQ(mime_type("app.js"), std::string("text/javascript; charset=utf-8"));
    CHECK_EQ(mime_type("noext"), std::string("application/octet-stream"));
}

TEST_CASE("config: console section") {
    auto c = parse_config("pools:\n  - {name: a, model_file: a.gguf, nodes_per_group: 1, groups: 1}\n", std::nullopt);
    CHECK(c.console.enabled);
    CHECK(!c.console.allow_exec);  // exec is opt-in
    CHECK_EQ(c.console_model(), std::string("a"));
    auto d = parse_config(R"(
console:
  allow_exec: true
  model: b
  router_url: http://192.168.1.10:8000
  max_tokens: 512
pools:
  - {name: a, model_file: a.gguf, nodes_per_group: 1, groups: 1}
  - {name: b, model_file: b.gguf, nodes_per_group: 1, groups: 1}
)",
                          std::nullopt);
    CHECK(d.console.allow_exec);
    CHECK_EQ(d.console_model(), std::string("b"));
    CHECK_EQ(d.console.max_tokens, 512);
    CHECK(d.warnings.empty());
    auto e = parse_config("console: {model: nope}\n", std::nullopt);
    CHECK(!e.warnings.empty());
    CHECK_THROWS(parse_config("console: {max_tokens: 0}\n", std::nullopt));
}
