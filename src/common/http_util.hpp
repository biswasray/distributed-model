// HTTP helpers on top of cpp-httplib: auth, JSON replies, JSON client calls, TCP probes.
#pragma once

#include <httplib.h>

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "common/net.hpp"

namespace xgi {

using json = nlohmann::json;

// {"error": {"message": ..., "type": ..., "code": ...}}  (05-protocols-and-data.md §4)
json error_body(int code, const std::string& message, const std::string& type);
void reply_json(httplib::Response& res, int status, const json& body);
void reply_error(httplib::Response& res, int code, const std::string& message, const std::string& type);

// X-Cluster-Token check. When `token` is empty, auth is off. On failure, writes 401 and returns false.
bool check_cluster_token(const httplib::Request& req, httplib::Response& res, const std::string& token);

// Parses the request body as JSON. An empty body gives an empty object. On failure writes 400.
std::optional<json> parse_body(const httplib::Request& req, httplib::Response& res);

struct BaseUrl {
    std::string scheme = "http";
    std::string host;
    int port = 80;
    std::string origin() const;  // "http://host:port"
};
std::optional<BaseUrl> parse_base_url(const std::string& url);

struct HttpResult {
    bool ok = false;     // transport succeeded and status is 2xx
    int status = 0;      // 0 = transport error
    json body;           // parsed JSON (null if not JSON)
    std::string error;   // transport error or non-2xx summary
};

// One JSON call with a total per-operation timeout. `body` is ignored for GET.
HttpResult http_call(const std::string& method, const std::string& base_url, const std::string& path,
                     const std::string& token, const json& body, double timeout_s);

}  // namespace xgi
