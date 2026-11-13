// mock-llama-server.exe: stands in for llama-server in tests. Accepts llama-server's flags,
// checks that every --rpc endpoint is reachable (like the real one), answers /health with 503
// while "loading" and 200 afterwards, and serves fake OpenAI completions (streaming or not).
//
// Extra flags: --mock-load-s N (default 2), --mock-token-ms N (default 20), --mock-tokens N (default 8)
#include <httplib.h>

#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "common/http_util.hpp"
#include "common/util.hpp"

using json = nlohmann::json;

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", rpc, model;
    int port = 8080, tokens = 8, token_ms = 20;
    double load_s = 2;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host") ++i;  // always 127.0.0.1: tests must not trigger firewall prompts
        else if (a == "--port") port = std::stoi(argv[++i]);
        else if (a == "--rpc") rpc = argv[++i];
        else if (a == "-m") model = argv[++i];
        else if (a == "--mock-load-s") load_s = std::stod(argv[++i]);
        else if (a == "--mock-token-ms") token_ms = std::stoi(argv[++i]);
        else if (a == "--mock-tokens") tokens = std::stoi(argv[++i]);
    }
    std::cout << "mock llama-server: model " << model << ", rpc [" << rpc << "]" << std::endl;
    for (const auto& ep : xgi::split(rpc, ',')) {
        if (ep.empty()) continue;
        auto colon = ep.rfind(':');
        if (!xgi::tcp_probe(ep.substr(0, colon), std::stoi(ep.substr(colon + 1)), 2000)) {
            std::cout << "failed to connect to RPC server at " << ep << std::endl;
            return 1;
        }
    }
    const double ready_at = xgi::mono_now() + load_s;

    httplib::Server s;
    s.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
        if (xgi::mono_now() < ready_at) {
            res.status = 503;
            res.set_content(R"({"error":{"code":503,"message":"Loading model","type":"unavailable_error"}})", "application/json");
        } else {
            res.set_content(R"({"status":"ok"})", "application/json");
        }
    });
    auto complete = [&](const httplib::Request& req, httplib::Response& res) {
        if (xgi::mono_now() < ready_at) {
            res.status = 503;
            res.set_content(R"({"error":{"code":503,"message":"Loading model"}})", "application/json");
            return;
        }
        json body = json::parse(req.body, nullptr, false);
        bool stream = body.is_object() && body.value("stream", false);
        bool chat = req.path == "/v1/chat/completions";
        std::cout << "request " << req.path << (stream ? " (stream)" : "") << std::endl;
        if (!stream) {
            std::this_thread::sleep_for(std::chrono::milliseconds(token_ms * tokens));
            std::string text = "Hello from mock llama-server on port " + std::to_string(port);
            json choice = chat ? json{{"index", 0}, {"message", {{"role", "assistant"}, {"content", text}}}, {"finish_reason", "stop"}}
                               : json{{"index", 0}, {"text", text}, {"finish_reason", "stop"}};
            res.set_content(json{{"object", chat ? "chat.completion" : "text_completion"}, {"choices", {choice}}}.dump(),
                            "application/json");
            return;
        }
        res.set_chunked_content_provider("text/event-stream", [=](size_t, httplib::DataSink& sink) {
            for (int i = 0; i < tokens; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(token_ms));
                json delta = chat ? json{{"choices", {{{"index", 0}, {"delta", {{"content", "tok" + std::to_string(i) + " "}}}}}}}
                                  : json{{"choices", {{{"index", 0}, {"text", "tok" + std::to_string(i) + " "}}}}};
                std::string ev = "data: " + delta.dump() + "\n\n";
                if (!sink.write(ev.data(), ev.size())) {
                    std::cout << "client went away" << std::endl;
                    return false;
                }
            }
            std::string done = "data: [DONE]\n\n";
            sink.write(done.data(), done.size());
            sink.done();
            return true;
        });
    };
    s.Post("/v1/chat/completions", complete);
    s.Post("/v1/completions", complete);
    std::cout << "mock llama-server listening on " << host << ":" << port << std::endl;
    if (!s.listen(host, port)) {
        std::cout << "cannot bind " << host << ":" << port << std::endl;
        return 1;
    }
    return 0;
}
