// xgi.exe — XGI (Extended General Intelligence): the Qwen Cluster core.
// Runs the coordinator (:8100), the router (:8000), or both. See architecture/03 and 04.
#include <windows.h>

#include <iostream>
#include <memory>
#include <thread>

#include "common/cli.hpp"
#include "common/config.hpp"
#include "common/http_util.hpp"
#include "common/log.hpp"
#include "common/util.hpp"
#include "xgi/coordinator.hpp"
#include "xgi/router.hpp"

namespace {

std::vector<httplib::Server*> g_servers;

BOOL WINAPI on_console_event(DWORD) {
    for (auto* s : g_servers) s->stop();
    return TRUE;
}

const char* kUsage = R"(xgi - XGI (Extended General Intelligence): Qwen Cluster coordinator and router

Usage:
  xgi coordinator  --config cluster.yaml [--host H] [--port P]   Registry, placement, lifecycle (:8100)
  xgi router       --config cluster.yaml [--host H] [--port P]   OpenAI-compatible gateway (:8000)
  xgi serve        --config cluster.yaml                          Both in one process
  xgi check-config --config cluster.yaml                          Validate and summarise the config
  xgi --version

Common options:
  --config PATH       cluster.yaml (default .\cluster.yaml)
  --log-dir DIR       Log folder (default .\logs)
  --log-level LEVEL   debug | info | warn | error (default info)

The cluster token comes from env QWEN_CLUSTER_TOKEN, then cluster_token in cluster.yaml.
)";

void configure_server(httplib::Server& s, size_t threads) {
    s.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };
    s.set_read_timeout(30, 0);
    s.set_write_timeout(30, 0);
    s.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "internal error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            msg = e.what();
        } catch (...) {
        }
        xgi::reply_error(res, 500, msg, "internal");
    });
}

int check_config(const xgi::ClusterConfig& c) {
    std::cout << "cluster.yaml OK\n";
    for (const auto& w : c.warnings) std::cout << "  warning: " << w << "\n";
    std::cout << std::format("  auth: cluster token {}, router api_keys {}\n", c.cluster_token.empty() ? "NOT SET" : "set",
                             c.router.api_keys.empty() ? "open" : std::to_string(c.router.api_keys.size()));
    for (const auto& p : c.pools)
        std::cout << std::format("  pool {:<16} {} group(s) x {} node(s), context {}, slots {}  model {}\n", p.name,
                                 p.groups, p.nodes_per_group, p.context, c.slots_for(p), p.model_file);
    std::cout << std::format("  spares {}  -> needs {} live nodes\n", c.spares, c.total_nodes_requested() + c.spares);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    xgi::CliArgs args(argc, argv);
    if (args.has("--version")) {
        std::cout << "xgi " << xgi::kVersion << "\n";
        return 0;
    }
    if (args.has("--help") || args.has("-h") || args.positional().empty()) {
        std::cout << kUsage;
        return args.positional().empty() && !args.has("--help") && !args.has("-h") ? 2 : 0;
    }
    const std::string cmd = args.positional().front();
    if (cmd != "coordinator" && cmd != "router" && cmd != "serve" && cmd != "check-config") {
        std::cerr << "xgi: unknown command '" << cmd << "'\n\n" << kUsage;
        return 2;
    }
    if (auto bad = args.unknown({"--config", "--host", "--port", "--log-dir", "--log-level"}); !bad.empty()) {
        std::cerr << "xgi: unknown option " << bad.front() << "\n\n" << kUsage;
        return 2;
    }

    xgi::ClusterConfig cfg;
    try {
        cfg = xgi::load_config(xgi::widen(args.get_or("--config", "cluster.yaml")));
    } catch (const std::exception& e) {
        std::cerr << "xgi: " << e.what() << "\n";
        return 1;
    }
    if (cmd == "check-config") return check_config(cfg);

    const bool run_coord = cmd == "coordinator" || cmd == "serve";
    const bool run_router = cmd == "router" || cmd == "serve";
    try {
        if (cmd == "coordinator") {
            cfg.coordinator.host = args.get_or("--host", cfg.coordinator.host);
            cfg.coordinator.port = args.get_int("--port", cfg.coordinator.port);
        } else if (cmd == "router") {
            cfg.router.host = args.get_or("--host", cfg.router.host);
            cfg.router.port = args.get_int("--port", cfg.router.port);
        } else if (args.has("--host") || args.has("--port")) {
            std::cerr << "xgi serve: set ports in cluster.yaml (coordinator.port, router.port)\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "xgi: " << e.what() << "\n";
        return 2;
    }

    std::string log_name = cmd == "serve" ? "xgi" : "xgi-" + cmd;
    xgi::log::init(log_name, xgi::widen(args.get_or("--log-dir", ".\\logs")),
                   xgi::log::parse_level(args.get_or("--log-level", "info")));
    xgi::ensure_winsock();
    for (const auto& w : cfg.warnings) xgi::log::warn("config: {}", w);
    if (cfg.cluster_token.empty())
        xgi::log::warn("no cluster token set (QWEN_CLUSTER_TOKEN): control plane is UNAUTHENTICATED");

    std::unique_ptr<xgi::Coordinator> coordinator;
    std::unique_ptr<xgi::Router> router;
    httplib::Server coord_srv, router_srv;

    if (run_coord) {
        coordinator = std::make_unique<xgi::Coordinator>(cfg);
        configure_server(coord_srv, 64);
        // Every endpoint except /health needs X-Cluster-Token (ADR-004).
        coord_srv.set_pre_routing_handler([&cfg](const httplib::Request& req, httplib::Response& res) {
            if (req.path == "/health") return httplib::Server::HandlerResponse::Unhandled;
            return xgi::check_cluster_token(req, res, cfg.cluster_token) ? httplib::Server::HandlerResponse::Unhandled
                                                                         : httplib::Server::HandlerResponse::Handled;
        });
        coordinator->register_routes(coord_srv);
        if (!coord_srv.bind_to_port(cfg.coordinator.host, cfg.coordinator.port)) {
            xgi::log::error("coordinator: cannot listen on {}:{}", cfg.coordinator.host, cfg.coordinator.port);
            return 1;
        }
        g_servers.push_back(&coord_srv);
        xgi::log::info("XGI {} coordinator listening on {}:{} ({} pool(s), spares {})", xgi::kVersion,
                       cfg.coordinator.host, cfg.coordinator.port, cfg.pools.size(), cfg.spares);
    }
    if (run_router) {
        router = std::make_unique<xgi::Router>(cfg);
        configure_server(router_srv, router->recommended_threads());
        router_srv.set_write_timeout(static_cast<time_t>(std::max(30.0, cfg.router.request_timeout_s)), 0);
        router->register_routes(router_srv);
        if (!router_srv.bind_to_port(cfg.router.host, cfg.router.port)) {
            xgi::log::error("router: cannot listen on {}:{}", cfg.router.host, cfg.router.port);
            return 1;
        }
        g_servers.push_back(&router_srv);
        xgi::log::info("XGI {} router listening on {}:{} -> coordinator {} (client auth {})", xgi::kVersion,
                       cfg.router.host, cfg.router.port, cfg.router.coordinator_url,
                       cfg.router.api_keys.empty() ? "OFF" : "on");
    }
    SetConsoleCtrlHandler(on_console_event, TRUE);

    if (coordinator) coordinator->start();
    if (router) router->start();

    std::thread coord_thread;
    if (run_coord && run_router) coord_thread = std::thread([&] { coord_srv.listen_after_bind(); });
    if (run_router)
        router_srv.listen_after_bind();
    else
        coord_srv.listen_after_bind();

    xgi::log::info("shutting down");
    for (auto* s : g_servers) s->stop();
    if (coord_thread.joinable()) coord_thread.join();
    if (router) router->stop();
    if (coordinator) coordinator->stop();
    return 0;
}
