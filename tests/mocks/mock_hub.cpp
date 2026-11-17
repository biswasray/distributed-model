// mock-hub: stands in for Hugging Face and GitHub in the download tests (ADR-011). XI talks to it
// when XI_HF_BASE, XI_GITHUB_BASE and XI_GITHUB_API_BASE point here.
//
//   GET /api/models/test/mock/tree/<ref>            HF file list (other repos: 401, like Hugging Face)
//   GET /test/mock/resolve/<ref>/<path>             HF file (Range requests work)
//   GET /api/repos/test/mock/releases/latest        GitHub release "v1"
//   GET /test/mock/releases/download/v1/<name>      302 -> /blobs/<name>, like GitHub's CDN redirect
//
// Files are deterministic bytes, served slowly (--chunk-ms per 32 KB) so a test can pause mid-way.
// Usage: mock-hub [--port 18900] [--chunk-ms 20]
#include <httplib.h>

#include <chrono>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

using json = nlohmann::json;

int main(int argc, char** argv) {
    int port = 18900, chunk_ms = 20;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port") port = std::stoi(argv[++i]);
        else if (a == "--chunk-ms") chunk_ms = std::stoi(argv[++i]);
    }
    const std::map<std::string, size_t> files{
        {"mock-a.gguf", 8'000'000},
        {"mock-c.gguf", 300'000},
        {"README.md", 64},
        {"split/mock-b-00001-of-00002.gguf", 3'000'000},
        {"split/mock-b-00002-of-00002.gguf", 2'000'000},
        {"mock-gh.gguf", 500'000},  // the GitHub release asset
    };
    auto serve = [&files, chunk_ms](const std::string& name, httplib::Response& res) {
        auto it = files.find(name);
        if (it == files.end()) {
            res.status = 404;
            res.set_content("Entry not found", "text/plain");
            return;
        }
        res.set_content_provider(it->second, "application/octet-stream",
                                 [chunk_ms](size_t offset, size_t length, httplib::DataSink& sink) {
                                     std::string buf(std::min<size_t>(length, 32 * 1024), '\0');
                                     for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<char>((offset + i) % 251);
                                     std::this_thread::sleep_for(std::chrono::milliseconds(chunk_ms));
                                     return sink.write(buf.data(), buf.size());
                                 });
    };

    httplib::Server s;
    s.Get(R"(/api/models/([^/]+)/([^/]+)/tree/([^/?]+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (std::string(req.matches[1]) + "/" + std::string(req.matches[2]) != "test/mock") {
            res.status = 401;
            res.set_content(R"({"error":"Invalid username or password."})", "application/json");
            return;
        }
        json out = json::array({{{"type", "directory"}, {"path", "split"}}});
        for (const auto& [name, size] : files)
            if (name != "mock-gh.gguf") out.push_back({{"type", "file"}, {"path", name}, {"size", size}});
        res.set_content(out.dump(), "application/json");
    });
    s.Get(R"(/test/mock/resolve/([^/]+)/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        serve(req.matches[2], res);
    });
    s.Get("/api/repos/test/mock/releases/latest", [&](const httplib::Request&, httplib::Response& res) {
        json out{{"tag_name", "v1"},
                 {"assets", json::array({{{"name", "mock-gh.gguf"}, {"size", files.at("mock-gh.gguf")}}})}};
        res.set_content(out.dump(), "application/json");
    });
    s.Get(R"(/api/repos/([^/]+)/([^/]+)/releases/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content(R"({"message":"Not Found"})", "application/json");
    });
    s.Get(R"(/test/mock/releases/download/v1/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        res.set_redirect("/blobs/" + std::string(req.matches[1]));
    });
    s.Get(R"(/blobs/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) { serve(req.matches[1], res); });

    std::cout << "mock hub on 127.0.0.1:" << port << std::endl;
    if (!s.listen("127.0.0.1", port)) {
        std::cerr << "cannot listen on " << port << std::endl;
        return 1;
    }
    return 0;
}
