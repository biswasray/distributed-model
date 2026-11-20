#include "common/hub.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>

#include "common/util.hpp"

namespace xgi::hub {

namespace {

bool name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'; }

bool valid_name(std::string_view s, size_t max) {
    if (s.empty() || s.size() > max || s == "." || s == "..") return false;
    return std::all_of(s.begin(), s.end(), name_char);
}

std::string trim_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace

std::optional<Source> parse_source(std::string_view s) {
    std::string l = to_lower(std::string(s));
    if (l == "huggingface" || l == "hf") return Source::huggingface;
    if (l == "github" || l == "gh") return Source::github;
    return std::nullopt;
}

const char* source_name(Source s) { return s == Source::huggingface ? "huggingface" : "github"; }

bool valid_repo(std::string_view repo) {
    size_t slash = repo.find('/');
    if (slash == std::string_view::npos || repo.find('/', slash + 1) != std::string_view::npos) return false;
    return valid_name(repo.substr(0, slash), 96) && valid_name(repo.substr(slash + 1), 96);
}

bool valid_ref(std::string_view ref) {
    if (ref.empty() || ref.size() > 128 || ref.front() == '/' || ref.back() == '/') return false;
    if (ref.find("..") != std::string_view::npos || ref.find("//") != std::string_view::npos) return false;
    return std::all_of(ref.begin(), ref.end(), [](char c) { return name_char(c) || c == '/' || c == '+'; });
}

bool valid_file_path(std::string_view path, bool allow_dirs) {
    if (path.empty() || path.size() > 512) return false;
    for (char c : path) {
        auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            return false;
        if (c == '/' && !allow_dirs) return false;
    }
    for (const auto& seg : split(path, '/')) {
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (seg.front() == ' ' || seg.back() == ' ' || seg.back() == '.') return false;  // Windows can't keep these
    }
    return true;
}

std::string encode_segment(std::string_view s) {
    std::string out;
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '.' || c == '_' || c == '~') out += c;
        else out += std::format("%{:02X}", static_cast<unsigned>(u));
    }
    return out;
}

std::string encode_path(std::string_view path) {
    std::vector<std::string> parts;
    for (const auto& seg : split(path, '/')) parts.push_back(encode_segment(seg));
    return join(parts, "/");
}

Bases Bases::from_env() {
    Bases b;
    if (auto v = get_env("XI_HF_BASE"); v && !v->empty()) b.huggingface = trim_slash(*v);
    if (auto v = get_env("XI_GITHUB_BASE"); v && !v->empty()) b.github = trim_slash(*v);
    if (auto v = get_env("XI_GITHUB_API_BASE"); v && !v->empty()) b.github_api = trim_slash(*v);
    return b;
}

std::string default_ref(Source s) { return s == Source::huggingface ? "main" : "latest"; }

std::string file_url(const Bases& b, Source s, const std::string& repo, const std::string& ref, const std::string& path) {
    if (s == Source::huggingface)
        return std::format("{}/{}/resolve/{}/{}", b.huggingface, repo, encode_segment(ref), encode_path(path));
    return std::format("{}/{}/releases/download/{}/{}", b.github, repo, encode_segment(ref), encode_segment(path));
}

std::string listing_url(const Bases& b, Source s, const std::string& repo, const std::string& ref) {
    if (s == Source::huggingface)
        return std::format("{}/api/models/{}/tree/{}?recursive=true", b.huggingface, repo, encode_segment(ref));
    if (ref.empty() || ref == "latest") return std::format("{}/repos/{}/releases/latest", b.github_api, repo);
    return std::format("{}/repos/{}/releases/tags/{}", b.github_api, repo, encode_segment(ref));
}

nlohmann::json Listing::to_json() const {
    auto arr = nlohmann::json::array();
    for (const auto& f : files)
        arr.push_back({{"path", f.path}, {"size_bytes", f.size ? nlohmann::json(*f.size) : nlohmann::json(nullptr)}});
    return {{"ref", ref}, {"files", arr}};
}

Listing parse_listing(Source s, const nlohmann::json& body, const std::string& requested_ref) {
    Listing l;
    auto size_of = [](const nlohmann::json& j, const char* key) -> std::optional<std::uint64_t> {
        if (j.is_object() && j.contains(key) && j[key].is_number_unsigned()) return j[key].get<std::uint64_t>();
        if (j.is_object() && j.contains(key) && j[key].is_number_integer() && j[key].get<long long>() >= 0)
            return static_cast<std::uint64_t>(j[key].get<long long>());
        return std::nullopt;
    };
    if (s == Source::huggingface) {
        if (!body.is_array()) throw std::runtime_error("unexpected Hugging Face reply (expected a file list)");
        l.ref = requested_ref.empty() ? "main" : requested_ref;
        for (const auto& e : body) {
            if (!e.is_object() || e.value("type", "") != "file" || !e["path"].is_string()) continue;
            HubFile f{e["path"].get<std::string>(), size_of(e, "size")};
            // LFS files: "size" is the real size too, but lfs.size is authoritative when present.
            if (e.contains("lfs") && e["lfs"].is_object())
                if (auto ls = size_of(e["lfs"], "size")) f.size = ls;
            l.files.push_back(std::move(f));
        }
    } else {
        if (!body.is_object() || !body["tag_name"].is_string())
            throw std::runtime_error("unexpected GitHub reply (expected a release)");
        l.ref = body["tag_name"].get<std::string>();
        if (body.contains("assets") && body["assets"].is_array())
            for (const auto& a : body["assets"])
                if (a.is_object() && a["name"].is_string()) l.files.push_back({a["name"].get<std::string>(), size_of(a, "size")});
    }
    std::sort(l.files.begin(), l.files.end(), [](const HubFile& a, const HubFile& b) { return a.path < b.path; });
    return l;
}

std::string hub_error_message(const nlohmann::json& body) {
    if (!body.is_object()) return {};
    if (body.contains("error") && body["error"].is_string()) return body["error"].get<std::string>();
    if (body.contains("message") && body["message"].is_string()) return body["message"].get<std::string>();
    return {};
}

bool is_gguf(std::string_view path) { return ends_with_ci(path, ".gguf"); }

std::optional<HubRef> parse_hub_url(std::string_view url) {
    std::string u(url);
    if (auto q = u.find_first_of("?#"); q != std::string::npos) u.resize(q);
    for (const char* scheme : {"https://", "http://"})
        if (u.rfind(scheme, 0) == 0) u = u.substr(std::string_view(scheme).size());
    if (u.rfind("www.", 0) == 0) u = u.substr(4);
    auto parts = split(u, '/');
    if (parts.size() < 6) return std::nullopt;
    std::string host = to_lower(parts[0]);
    HubRef r;
    r.repo = parts[1] + "/" + parts[2];
    std::vector<std::string> rest(parts.begin() + 5, parts.end());
    if (host == "huggingface.co" || host == "hf.co") {
        if (parts[3] != "blob" && parts[3] != "resolve") return std::nullopt;
        r.source = Source::huggingface;
        r.ref = parts[4];
        r.file = join(rest, "/");
    } else if (host == "github.com") {
        if (parts.size() != 7 || parts[3] != "releases" || parts[4] != "download") return std::nullopt;
        r.source = Source::github;
        r.ref = parts[5];
        r.file = parts[6];
    } else {
        return std::nullopt;
    }
    // Percent-decode what the browser encoded (e.g. %2B).
    auto decode = [](const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                out += s[i];
            }
        }
        return out;
    };
    r.ref = decode(r.ref);
    r.file = decode(r.file);
    if (!valid_repo(r.repo) || !valid_ref(r.ref) || !valid_file_path(r.file, r.source == Source::huggingface))
        return std::nullopt;
    return r;
}

bool is_lan_ipv4(std::string_view host) {
    int oct[4];
    size_t i = 0;
    for (int k = 0; k < 4; ++k) {
        if (k > 0) {
            if (i >= host.size() || host[i] != '.') return false;
            ++i;
        }
        size_t start = i;
        int v = 0;
        while (i < host.size() && host[i] >= '0' && host[i] <= '9' && i - start < 3) v = v * 10 + (host[i++] - '0');
        if (i == start || v > 255 || (i - start > 1 && host[start] == '0')) return false;
        oct[k] = v;
    }
    if (i != host.size()) return false;
    return oct[0] == 10 || oct[0] == 127 || (oct[0] == 172 && oct[1] >= 16 && oct[1] <= 31) ||
           (oct[0] == 192 && oct[1] == 168) || (oct[0] == 169 && oct[1] == 254);
}

std::optional<std::string> normalize_peer_url(std::string_view url) {
    constexpr std::string_view scheme = "http://";
    if (url.size() <= scheme.size() || to_lower(std::string(url.substr(0, scheme.size()))) != scheme) return std::nullopt;
    std::string rest(url.substr(scheme.size()));
    if (!rest.empty() && rest.back() == '/') rest.pop_back();
    size_t colon = rest.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string host = rest.substr(0, colon), port = rest.substr(colon + 1);
    if (!is_lan_ipv4(host) || port.empty() || port.size() > 5) return std::nullopt;
    if (!std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    int p = std::stoi(port);
    if (p < 1 || p > 65535) return std::nullopt;
    return std::format("http://{}:{}", host, p);
}

std::string peer_listing_url(const std::string& base) { return trim_slash(base) + "/models"; }

std::string peer_file_url(const std::string& base, const std::string& path) {
    return trim_slash(base) + "/models/file/" + encode_path(path);
}

Listing parse_peer_listing(const nlohmann::json& body) {
    if (!body.is_object() || !body.contains("files") || !body["files"].is_array())
        throw std::runtime_error("unexpected reply from the peer (expected a model file list)");
    Listing l;
    for (const auto& e : body["files"]) {
        if (!e.is_object() || !e.contains("path") || !e["path"].is_string()) continue;
        HubFile f{e["path"].get<std::string>(), std::nullopt};
        if (e.contains("size_bytes") && e["size_bytes"].is_number_integer() && e["size_bytes"].get<long long>() >= 0)
            f.size = e["size_bytes"].get<std::uint64_t>();
        l.files.push_back(std::move(f));
    }
    std::sort(l.files.begin(), l.files.end(), [](const HubFile& a, const HubFile& b) { return a.path < b.path; });
    return l;
}

std::string format_bytes(double b) {
    if (b < 0) b = 0;
    if (b >= 1e9) return std::format("{:.2f} GB", b / 1e9);
    if (b >= 1e6) return std::format("{:.1f} MB", b / 1e6);
    if (b >= 1e3) return std::format("{:.0f} KB", b / 1e3);
    return std::format("{:.0f} B", b);
}

std::string format_duration(double s) {
    if (s < 0) s = 0;
    auto t = static_cast<long long>(s + 0.5);
    if (t < 60) return std::format("{}s", t);
    if (t < 3600) return std::format("{}m{:02}s", t / 60, t % 60);
    return std::format("{}h{:02}m", t / 3600, (t % 3600) / 60);
}

}  // namespace xgi::hub
