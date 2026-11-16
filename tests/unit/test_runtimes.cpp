#include <algorithm>
#include <random>

#include "common/platform.hpp"
#include "testing.hpp"
#include "xi/runtimes.hpp"

using namespace xi;

namespace {
bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
}  // namespace

TEST_CASE("xi exec: request validation") {
    CHECK_THROWS(ExecRequest::from_json(json::object(), 300));
    CHECK_THROWS(ExecRequest::from_json(json{{"language", "python"}}, 300));
    CHECK_THROWS(ExecRequest::from_json(json{{"language", "python"}, {"code", ""}}, 300));
    CHECK_THROWS(ExecRequest::from_json(json{{"language", "python"}, {"code", "x"}, {"timeout_s", 0}}, 300));
    CHECK_THROWS(ExecRequest::from_json(
        json{{"language", "python"}, {"code", std::string(ExecRequest::kMaxCodeBytes + 1, 'x')}}, 300));
    auto r = ExecRequest::from_json(json{{"language", "python"}, {"code", "print(1)"}, {"timeout_s", 9999}}, 300);
    CHECK_EQ(r.timeout_s, 300.0);  // capped by --exec-max-timeout-s
    CHECK_EQ(ExecRequest::from_json(json{{"language", "go"}, {"code", "x"}}, 300).timeout_s, 60.0);
}

TEST_CASE("xi exec: command lines per language") {
    fs::path src = "job/main.x", bin = "job/main";
    auto py = build_exec_plan(*find_spec("python"), "py3", src, bin);
    CHECK(!py.compile);
    CHECK(has(py.run, "utf8") && py.run.front() == "py3");
    auto c = build_exec_plan(*find_spec("c"), "gcc", src, bin);
    REQUIRE(c.compile.has_value());
    CHECK(has(*c.compile, "-o"));
    CHECK_EQ(c.run.size(), size_t{1});
    auto go = build_exec_plan(*find_spec("go"), "go", src, bin);
    CHECK(has(go.run, "run"));
    auto ps = build_exec_plan(*find_spec("pwsh"), "pwsh", src, bin);
    CHECK(has(ps.run, "-File") && has(ps.run, "-NonInteractive"));
    CHECK(find_spec("cobol") == nullptr);
    // OS-specific shells only where they exist.
    CHECK((find_spec("cmd") != nullptr) == (xgi::kPlatformName == "windows"));
    CHECK((find_spec("sh") != nullptr) == (xgi::kPlatformName != "windows"));
}

TEST_CASE("xi exec: output cleanup") {
    auto out = clean_output({"[xi] started pid 12: x", "hello", std::string(10, 'a'), "[xi] stopped pid 12"}, 5);
    REQUIRE(out.size() == 2);
    CHECK_EQ(out[0], std::string("hello"));
    CHECK(out[1].rfind("aaaaa", 0) == 0 && out[1].find("truncated") != std::string::npos);
    CHECK_EQ(first_line({"", "   ", "  Python 3.12.1  ", "x"}), std::string("Python 3.12.1"));
    CHECK_EQ(first_line({}), std::string(""));
}

TEST_CASE("xi exec: runs a script with a real shell") {
    // cmd.exe on Windows, sh elsewhere: both always exist, so this runs on every machine.
    const bool win = xgi::kPlatformName == "windows";
    const std::string id = win ? "cmd" : "sh";
    auto exe = find_on_path(id);
    REQUIRE(exe.has_value());
    Runtime rt{id, id, "", *exe};
    fs::path dir = fs::temp_directory_path() / ("xgi-exec-test-" + std::to_string(std::random_device{}()));
    fs::path log = dir / "exec.log";
    fs::create_directories(dir);

    ExecRequest ok;
    ok.language = id;
    ok.code = win ? "@echo off\necho hello from cmd\necho second line\nexit /b 3\n"
                  : "echo hello from sh\necho second line\nexit 3\n";
    json r = run_job(rt, ok, dir, log);
    CHECK_EQ(r["exit_code"].get<long long>(), 3LL);
    CHECK(!r["timed_out"].get<bool>());
    REQUIRE(r["output"].is_array() && r["output"].size() == 2);
    CHECK(r["output"][0].get<std::string>().find("hello from") == 0);
    CHECK(!fs::exists(dir / ".jobs") || fs::is_empty(dir / ".jobs"));  // job folder removed

    ExecRequest slow;
    slow.language = id;
    slow.code = win ? "@echo off\necho start\nping -n 30 127.0.0.1 >nul\necho never\n" : "echo start\nsleep 30\necho never\n";
    slow.timeout_s = 1.5;
    json t = run_job(rt, slow, dir, log);
    CHECK(t["timed_out"].get<bool>());
    CHECK(t["duration_s"].get<double>() < 10);

    std::error_code ec;
    fs::remove_all(dir, ec);
}
