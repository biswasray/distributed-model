#include "common/http_util.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <format>
#include <mutex>

#include "common/util.hpp"

namespace xgi {

json error_body(int code, const std::string& message, const std::string& type) {
    return json{{"error", {{"message", message}, {"type", type}, {"code", code}}}};
}

void reply_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void reply_error(httplib::Response& res, int code, const std::string& message, const std::string& type) {
    reply_json(res, code, error_body(code, message, type));
}

bool check_cluster_token(const httplib::Request& req, httplib::Response& res, const std::string& token) {
    if (token.empty()) return true;
    if (secure_equals(req.get_header_value(kTokenHeader), token)) return true;
    reply_error(res, 401, "missing or invalid X-Cluster-Token", "auth");
    return false;
}

std::optional<json> parse_body(const httplib::Request& req, httplib::Response& res) {
    if (req.body.empty()) return json::object();
    try {
        return json::parse(req.body);
    } catch (const json::parse_error& e) {
        reply_error(res, 400, std::string("invalid JSON body: ") + e.what(), "invalid_request");
        return std::nullopt;
    }
}

std::string BaseUrl::origin() const { return std::format("{}://{}:{}", scheme, host, port); }

std::optional<BaseUrl> parse_base_url(const std::string& url) {
    BaseUrl u;
    std::string rest = url;
    auto pos = rest.find("://");
    if (pos != std::string::npos) {
        u.scheme = to_lower(rest.substr(0, pos));
        rest = rest.substr(pos + 3);
    }
    if (u.scheme != "http") return std::nullopt;  // the cluster is plain HTTP on the LAN
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        u.host = rest.substr(0, colon);
        try {
            u.port = std::stoi(rest.substr(colon + 1));
        } catch (...) {
            return std::nullopt;
        }
    } else {
        u.host = rest;
    }
    if (u.host.empty()) return std::nullopt;
    return u;
}

HttpResult http_call(const std::string& method, const std::string& base_url, const std::string& path,
                     const std::string& token, const json& body, double timeout_s) {
    HttpResult out;
    auto url = parse_base_url(base_url);
    if (!url) {
        out.error = "invalid URL " + base_url;
        return out;
    }
    httplib::Client cli(url->host, url->port);
    auto secs = static_cast<time_t>(timeout_s);
    auto usecs = static_cast<time_t>((timeout_s - static_cast<double>(secs)) * 1e6);
    cli.set_connection_timeout(secs, usecs);
    cli.set_read_timeout(secs, usecs);
    cli.set_write_timeout(secs, usecs);
    cli.set_keep_alive(false);
    httplib::Headers headers;
    if (!token.empty()) headers.emplace(kTokenHeader, token);

    httplib::Result r;
    if (method == "GET") {
        r = cli.Get(path, headers);
    } else {
        std::string payload = body.is_null() ? std::string("{}") : body.dump();
        r = cli.Post(path, headers, payload, "application/json");
    }
    if (!r) {
        out.error = httplib::to_string(r.error());
        return out;
    }
    out.status = r->status;
    out.ok = r->status >= 200 && r->status < 300;
    if (!r->body.empty()) out.body = json::parse(r->body, nullptr, /*allow_exceptions=*/false);
    if (out.body.is_discarded()) out.body = nullptr;
    if (!out.ok) {
        std::string detail;
        if (out.body.is_object() && out.body.contains("error")) {
            const auto& e = out.body["error"];
            detail = e.is_object() ? e.value("message", e.dump()) : e.dump();
        } else {
            detail = r->body.substr(0, 200);
        }
        out.error = std::format("HTTP {} {}", r->status, detail);
    }
    return out;
}

void ensure_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

bool tcp_probe(const std::string& host, int port, int timeout_ms) {
    ensure_winsock();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
    bool ok = false;
    for (addrinfo* ai = res; ai && !ok; ai = ai->ai_next) {
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int rc = connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc == 0) {
            ok = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(s, &wfds);
            FD_SET(s, &efds);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (select(0, nullptr, &wfds, &efds, &tv) > 0 && FD_ISSET(s, &wfds)) ok = true;
        }
        closesocket(s);
    }
    freeaddrinfo(res);
    return ok;
}

}  // namespace xgi
