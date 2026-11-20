#include "common/hub.hpp"
#include "common/platform.hpp"
#include "testing.hpp"
#include "xgi/download_cli.hpp"
#include "xi/downloads.hpp"

using namespace xgi::hub;
using nlohmann::json;

TEST_CASE("hub: sources, repos, refs and file paths") {
    CHECK(parse_source("HF") == Source::huggingface);
    CHECK(parse_source("github") == Source::github);
    CHECK(!parse_source("gitlab"));
    CHECK(valid_repo("Qwen/Qwen2.5-7B-Instruct-GGUF"));
    CHECK(!valid_repo("Qwen"));
    CHECK(!valid_repo("a/b/c"));
    CHECK(!valid_repo("../x"));
    CHECK(!valid_repo("a/.."));
    CHECK(!valid_repo("a b/c"));
    CHECK(valid_ref("main") && valid_ref("v1.2+build") && valid_ref("refs/pr/1"));
    CHECK(!valid_ref("") && !valid_ref("a/../b") && !valid_ref("/main") && !valid_ref("a b"));
    CHECK(valid_file_path("model-q4_k_m.gguf", false));
    CHECK(valid_file_path("Q4_K_M/model-00001-of-00002.gguf", true));
    CHECK(!valid_file_path("Q4_K_M/model.gguf", false));  // GitHub assets have no folders
    CHECK(!valid_file_path("../evil.gguf", true));
    CHECK(!valid_file_path("a/./b.gguf", true));
    CHECK(!valid_file_path("/abs.gguf", true));
    CHECK(!valid_file_path("C:evil.gguf", true));
    CHECK(!valid_file_path("a\\b.gguf", true));
    CHECK(!valid_file_path("trailing.", true));
    CHECK(!valid_file_path(std::string("nul\x01.gguf"), true));
}

TEST_CASE("hub: URLs") {
    Bases b;
    CHECK_EQ(file_url(b, Source::huggingface, "Qwen/X-GGUF", "main", "Q4/x y+1.gguf"),
             std::string("https://huggingface.co/Qwen/X-GGUF/resolve/main/Q4/x%20y%2B1.gguf"));
    CHECK_EQ(file_url(b, Source::github, "o/r", "v1.0", "m.gguf"),
             std::string("https://github.com/o/r/releases/download/v1.0/m.gguf"));
    CHECK_EQ(listing_url(b, Source::huggingface, "o/r", "refs/pr/1"),
             std::string("https://huggingface.co/api/models/o/r/tree/refs%2Fpr%2F1?recursive=true"));
    CHECK_EQ(listing_url(b, Source::github, "o/r", "latest"), std::string("https://api.github.com/repos/o/r/releases/latest"));
    CHECK_EQ(listing_url(b, Source::github, "o/r", "b123"), std::string("https://api.github.com/repos/o/r/releases/tags/b123"));
}

TEST_CASE("hub: parse listings") {
    json hf = json::parse(R"([
        {"type":"directory","path":"Q4"},
        {"type":"file","path":"README.md","size":12},
        {"type":"file","path":"Q4/m-00001-of-00002.gguf","size":134,"lfs":{"oid":"x","size":5000000000}}
    ])");
    auto l = parse_listing(Source::huggingface, hf, "main");
    CHECK_EQ(l.ref, std::string("main"));
    REQUIRE(l.files.size() == 2);
    CHECK_EQ(l.files[0].path, std::string("Q4/m-00001-of-00002.gguf"));
    CHECK(l.files[0].size && *l.files[0].size == 5000000000ull);
    json gh = json::parse(R"({"tag_name":"b4500","assets":[{"name":"m.gguf","size":42}]})");
    auto g = parse_listing(Source::github, gh, "latest");
    CHECK_EQ(g.ref, std::string("b4500"));
    REQUIRE(g.files.size() == 1);
    CHECK(*g.files[0].size == 42);
    CHECK_THROWS(parse_listing(Source::github, json::array(), "latest"));
    CHECK_THROWS(parse_listing(Source::huggingface, json::object(), "main"));
    CHECK_EQ(hub_error_message(json{{"error", "Repo not found"}}), std::string("Repo not found"));
    CHECK_EQ(hub_error_message(json{{"message", "Not Found"}}), std::string("Not Found"));
}

TEST_CASE("hub: pasted links") {
    auto a = parse_hub_url("https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/blob/main/qwen2.5-7b-instruct-q4_k_m.gguf?download=true");
    REQUIRE(a.has_value());
    CHECK(a->source == Source::huggingface);
    CHECK_EQ(a->repo, std::string("Qwen/Qwen2.5-7B-Instruct-GGUF"));
    CHECK_EQ(a->ref, std::string("main"));
    CHECK_EQ(a->file, std::string("qwen2.5-7b-instruct-q4_k_m.gguf"));
    auto b = parse_hub_url("https://huggingface.co/o/r/resolve/abc123/Q4_K_M/m%2B1.gguf");
    REQUIRE(b.has_value());
    CHECK_EQ(b->file, std::string("Q4_K_M/m+1.gguf"));
    auto c = parse_hub_url("https://github.com/o/r/releases/download/v1/model.gguf");
    REQUIRE(c.has_value());
    CHECK(c->source == Source::github);
    CHECK_EQ(c->ref, std::string("v1"));
    CHECK(!parse_hub_url("https://example.com/o/r/resolve/main/x.gguf"));
    CHECK(!parse_hub_url("https://huggingface.co/o/r/tree/main"));
    CHECK(!parse_hub_url("https://huggingface.co/o/r/blob/main/../../etc/passwd"));
    CHECK(!parse_hub_url("https://github.com/o/r/archive/main.zip"));
}

TEST_CASE("hub: formatting") {
    CHECK_EQ(format_bytes(512), std::string("512 B"));
    CHECK_EQ(format_bytes(4.7e9), std::string("4.70 GB"));
    CHECK_EQ(format_bytes(820e6), std::string("820.0 MB"));
    CHECK_EQ(format_duration(45), std::string("45s"));
    CHECK_EQ(format_duration(271), std::string("4m31s"));
    CHECK_EQ(format_duration(7980), std::string("2h13m"));
    json job{{"node_id", "node-a"}, {"state", "downloading"}, {"done_bytes", 1200000000}, {"total_bytes", 2400000000},
             {"speed_bps", 5300000}, {"eta_s", 226}, {"current_file", 0}, {"files", json::array({1, 2})}};
    auto line = xgi::progress_line(job, 10);
    CHECK(line.find("node-a  [#####.....]") == 0);
    CHECK(line.find("50.0%") != std::string::npos);
    CHECK(line.find("5.3 MB/s") != std::string::npos);
    CHECK(line.find("ETA 3m46s") != std::string::npos);
    CHECK(line.find("file 1/2") != std::string::npos);
    job["total_bytes"] = nullptr;
    job["state"] = "paused";
    CHECK(xgi::progress_line(job, 0).find("?%  paused") != std::string::npos);
}

TEST_CASE("xi downloads: request validation and targets") {
    using xi::DownloadRequest;
    auto r = DownloadRequest::from_json(json{{"source", "hf"}, {"repo", "o/r"}, {"files", {"a.gguf", "a.gguf", "Q4/b.gguf"}}});
    CHECK_EQ(r.ref, std::string("main"));
    CHECK_EQ(r.files.size(), size_t{2});  // duplicates dropped
    CHECK_EQ(DownloadRequest::from_json(json{{"source", "github"}, {"repo", "o/r"}, {"files", {"m.gguf"}}}).ref,
             std::string("latest"));
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "ftp"}, {"repo", "o/r"}, {"files", {"a"}}}));
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "hf"}, {"repo", "o"}, {"files", {"a"}}}));
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "hf"}, {"repo", "o/r"}, {"files", json::array()}}));
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "hf"}, {"repo", "o/r"}, {"files", {"../x.gguf"}}}));
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "github"}, {"repo", "o/r"}, {"files", {"d/x.gguf"}}}));
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "hf"}, {"repo", "o/r"}, {"files", {"x"}}, {"dest_dir", "../up"}}));
    auto d = DownloadRequest::from_json(json{{"source", "hf"}, {"repo", "o/r"}, {"files", {"x"}}, {"dest_dir", "qwen/7b/"}});
    CHECK_EQ(d.dest_dir, std::string("qwen/7b"));

    std::filesystem::path models = "models";
    auto t = xi::download_target(models, "qwen", "Q4_K_M/m-00001-of-00002.gguf");
    REQUIRE(t.has_value());
    CHECK_EQ(xgi::generic_utf8(*t), std::string("models/qwen/m-00001-of-00002.gguf"));  // folder in the repo dropped
    CHECK(!xi::download_target(models, "..", "x.gguf"));
}

TEST_CASE("xi downloads: curl output") {
    auto o = xi::parse_curl_output({"[xi] started pid 1", "curl: (22) The requested URL returned error: 404", "404"});
    CHECK_EQ(o.http_status, 404);
    CHECK_EQ(o.error, std::string("(22) The requested URL returned error: 404"));
    CHECK_EQ(xi::parse_curl_output({"200"}).http_status, 200);
    CHECK_EQ(xi::parse_curl_output({"curl: (6) Could not resolve host: x", "000"}).http_status, 0);
}

TEST_CASE("lan copies: peer URLs stay inside the LAN (ADR-012)") {
    CHECK(is_lan_ipv4("192.168.1.27") && is_lan_ipv4("10.0.0.1") && is_lan_ipv4("172.16.5.4") && is_lan_ipv4("172.31.255.255"));
    CHECK(is_lan_ipv4("127.0.0.1") && is_lan_ipv4("169.254.10.1"));
    CHECK(!is_lan_ipv4("172.32.0.1") && !is_lan_ipv4("8.8.8.8") && !is_lan_ipv4("192.169.0.1") && !is_lan_ipv4("100.64.0.1"));
    CHECK(!is_lan_ipv4("192.168.1") && !is_lan_ipv4("192.168.1.256") && !is_lan_ipv4("192.168.01.1") && !is_lan_ipv4("lan-pc"));
    CHECK(!is_lan_ipv4("192.168.1.1.5") && !is_lan_ipv4(""));
    CHECK_EQ(normalize_peer_url("http://192.168.1.27:8200/").value_or(""), std::string("http://192.168.1.27:8200"));
    CHECK_EQ(normalize_peer_url("HTTP://127.0.0.1:8100").value_or(""), std::string("http://127.0.0.1:8100"));
    CHECK(!normalize_peer_url("https://192.168.1.27:8200"));       // agents speak plain http
    CHECK(!normalize_peer_url("http://192.168.1.27"));             // port required
    CHECK(!normalize_peer_url("http://192.168.1.27:8200/models")); // no path
    CHECK(!normalize_peer_url("http://user@192.168.1.27:8200"));
    CHECK(!normalize_peer_url("http://example.com:8200"));         // no hostnames
    CHECK(!normalize_peer_url("http://8.8.8.8:80"));               // not the LAN
    CHECK(!normalize_peer_url("http://192.168.1.27:99999"));
    CHECK_EQ(peer_listing_url("http://10.0.0.2:8200/"), std::string("http://10.0.0.2:8200/models"));
    CHECK_EQ(peer_file_url("http://10.0.0.2:8200", "qwen 3/m+1.gguf"),
             std::string("http://10.0.0.2:8200/models/file/qwen%203/m%2B1.gguf"));
    auto l = parse_peer_listing(json{{"files", {{{"path", "b.gguf"}, {"size_bytes", 5}}, {{"path", "a/a.gguf"}}}}});
    REQUIRE(l.files.size() == 2);
    CHECK_EQ(l.files[0].path, std::string("a/a.gguf"));
    CHECK(!l.files[0].size);
    CHECK_EQ(l.files[1].size.value_or(0), std::uint64_t{5});
    CHECK_THROWS(parse_peer_listing(json::array()));
}

TEST_CASE("lan copies: peer download requests and targets") {
    using xi::DownloadRequest;
    auto r = DownloadRequest::from_json(json{{"source", "peer"}, {"peer_url", "http://192.168.1.21:8200/"}, {"peer_name", "LAB-PC-01"},
                                             {"files", {"qwen3/Qwen3-30B-A3B-Q4_K_M.gguf"}}});
    CHECK(r.peer);
    CHECK_EQ(r.peer_url, std::string("http://192.168.1.21:8200"));
    CHECK_EQ(r.peer_name, std::string("LAB-PC-01"));
    auto x = DownloadRequest::from_json(json{{"source", "LAN"}, {"peer_url", "@xgi"}, {"files", {"m.gguf"}}});
    CHECK(x.peer && x.peer_url == "@xgi" && x.peer_name == "@xgi");
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "peer"}, {"files", {"m.gguf"}}}));  // no peer
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "peer"}, {"peer_url", "http://1.2.3.4:80"}, {"files", {"m.gguf"}}}));
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "peer"}, {"peer_url", "@xgi"}, {"files", {"notes.txt"}}}));
    CHECK_THROWS(DownloadRequest::from_json(json{{"source", "peer"}, {"peer_url", "@xgi"}, {"files", {"../m.gguf"}}}));
    // A LAN copy keeps the file's folder, so the pool's model_file is the same on the copy.
    auto t = xi::download_target("models", "", "qwen3/m.gguf", true);
    REQUIRE(t.has_value());
    CHECK_EQ(xgi::generic_utf8(*t), std::string("models/qwen3/m.gguf"));
    CHECK_EQ(xgi::generic_utf8(*xi::download_target("models", "copies", "qwen3/m.gguf", true)),
             std::string("models/copies/qwen3/m.gguf"));
}
