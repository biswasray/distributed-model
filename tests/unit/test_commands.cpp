#include <windows.h>

#include <fstream>

#include "common/util.hpp"
#include "testing.hpp"
#include "xi/commands.hpp"

using namespace xi;

namespace {

bool contains_seq(const std::vector<std::string>& v, const std::vector<std::string>& seq) {
    return std::search(v.begin(), v.end(), seq.begin(), seq.end()) != v.end();
}
bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("xgi-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                                            std::to_string(GetTickCount64()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void touch(const fs::path& rel) {
        fs::create_directories((path / rel).parent_path());
        std::ofstream(path / rel) << "GGUF";
    }
};

}  // namespace

TEST_CASE("xi: rpc-server command line") {
    auto a = build_rpc_args(50052, 6, true);
    CHECK(contains_seq(a, {"-H", "0.0.0.0", "-p", "50052", "-t", "6", "-c"}));
    CHECK(!contains(build_rpc_args(50052, 6, false), "-c"));
}

TEST_CASE("xi: llama-server command line") {
    auto r = HeadStartRequest::from_json(json{{"model_file", "m.gguf"},
                                              {"rpc_endpoints", {"10.0.0.2:50052", "10.0.0.3:50052"}},
                                              {"context", 4096},
                                              {"parallel", 2},
                                              {"tensor_split", {1, 1.5, 1}},
                                              {"extra_args", {"--metrics"}}});
    auto a = build_head_args("C:\\models\\m.gguf", 8080, r, 8);
    CHECK(contains_seq(a, {"-m", "C:\\models\\m.gguf"}));
    CHECK(contains_seq(a, {"--host", "0.0.0.0", "--port", "8080"}));
    CHECK(contains_seq(a, {"-c", "8192", "-np", "2"}));  // context x parallel
    CHECK(contains_seq(a, {"-t", "8", "-ngl", "99"}));
    CHECK(contains_seq(a, {"--rpc", "10.0.0.2:50052,10.0.0.3:50052"}));
    CHECK(contains_seq(a, {"--tensor-split", "1,1.5,1"}));
    CHECK_EQ(a.back(), std::string("--metrics"));

    auto single = HeadStartRequest::from_json(json{{"model_file", "m.gguf"}});
    auto b = build_head_args("m.gguf", 8080, single, 4);
    CHECK(!contains(b, "--rpc"));  // single-node pools
    CHECK(!contains(b, "--tensor-split"));
    CHECK(contains_seq(b, {"-c", "4096", "-np", "1"}));
}

TEST_CASE("xi: start request validation") {
    CHECK_THROWS(HeadStartRequest::from_json(json::object()));
    CHECK_THROWS(HeadStartRequest::from_json(json{{"model_file", "m.gguf"}, {"rpc_endpoints", "x"}}));
    CHECK_THROWS(HeadStartRequest::from_json(json{{"model_file", "m.gguf"}, {"parallel", 0}}));
    auto r = RpcStartRequest::from_json(json{{"threads", nullptr}, {"cache", false}});
    CHECK(!r.threads.has_value());
    CHECK(!r.cache);
    CHECK(RpcStartRequest::from_json(json::object()).cache);
}

TEST_CASE("xi: model paths stay inside the models dir (ADR-005)") {
    TempDir models;
    models.touch("a.gguf");
    models.touch("sub/b-00001-of-00002.gguf");
    models.touch("sub/notes.txt");
    CHECK(resolve_model_path(models.path, "a.gguf").has_value());
    CHECK(resolve_model_path(models.path, "sub/b-00001-of-00002.gguf").has_value());
    CHECK(resolve_model_path(models.path, "sub\\b-00001-of-00002.gguf").has_value());
    CHECK(!resolve_model_path(models.path, "missing.gguf").has_value());
    CHECK(!resolve_model_path(models.path, "../a.gguf").has_value());
    CHECK(!resolve_model_path(models.path, "sub/../../a.gguf").has_value());
    CHECK(!resolve_model_path(models.path, xgi::narrow((models.path / "a.gguf").wstring())).has_value());
    CHECK(!resolve_model_path(models.path, "sub/notes.txt").has_value());

    auto found = scan_models(models.path);
    REQUIRE(found.size() == 2);
    CHECK_EQ(found[0], std::string("a.gguf"));
    CHECK_EQ(found[1], std::string("sub/b-00001-of-00002.gguf"));
}

TEST_CASE("xi: Windows argument quoting") {
    CHECK(quote_windows_arg(L"plain") == L"plain");
    CHECK(quote_windows_arg(L"") == L"\"\"");
    CHECK(quote_windows_arg(L"C:\\Program Files\\x.exe") == L"\"C:\\Program Files\\x.exe\"");
    CHECK(quote_windows_arg(L"a \"b\"") == L"\"a \\\"b\\\"\"");
    CHECK(quote_windows_arg(L"dir with space\\") == L"\"dir with space\\\\\"");
    auto py = program_argv("mock.py", {"-p", "1"});
    CHECK_EQ(py.front(), std::string("python"));
}
