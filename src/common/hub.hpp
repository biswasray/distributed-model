// Model hubs: Hugging Face model repos and GitHub releases (05-protocols-and-data.md §3.3, ADR-011).
//
// Pure helpers shared by XI (which does the downloading, with the system curl) and the xgi CLI:
// validating what a request names, building the hub URLs, parsing hub listings and pasted URLs,
// and formatting progress. Nothing here touches the network.
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xgi::hub {

enum class Source { huggingface, github };

// "huggingface" | "hf" | "github" | "gh" (case-insensitive).
std::optional<Source> parse_source(std::string_view s);
const char* source_name(Source s);  // "huggingface" | "github"

// "owner/name": each part 1-96 of [A-Za-z0-9._-], not "." or "..".
bool valid_repo(std::string_view repo);
// A branch, tag or commit: 1-128 of [A-Za-z0-9._/+-], no "..", no leading or trailing '/'.
bool valid_ref(std::string_view ref);
// A relative file path inside a repo or release: '/'-separated (only if allow_dirs), no "." or
// ".." segments, no '\\', ':' or control characters, at most 512 bytes.
bool valid_file_path(std::string_view path, bool allow_dirs);

// Percent-encodes one URL path segment (keeps A-Z a-z 0-9 - . _ ~).
std::string encode_segment(std::string_view s);
// Encodes each '/'-separated segment of a path.
std::string encode_path(std::string_view path);

// Hub origins. Tests point these at a local mock with XI_HF_BASE, XI_GITHUB_BASE, XI_GITHUB_API_BASE.
struct Bases {
    std::string huggingface = "https://huggingface.co";
    std::string github = "https://github.com";
    std::string github_api = "https://api.github.com";
    static Bases from_env();
};

// Hugging Face: <hf>/<repo>/resolve/<ref>/<path>.  GitHub: <gh>/<repo>/releases/download/<tag>/<name>.
std::string file_url(const Bases& b, Source s, const std::string& repo, const std::string& ref, const std::string& path);
// Hugging Face: <hf>/api/models/<repo>/tree/<ref>?recursive=true
// GitHub: <api>/repos/<repo>/releases/latest, or .../releases/tags/<tag>
std::string listing_url(const Bases& b, Source s, const std::string& repo, const std::string& ref);
// The default ref: "main" (Hugging Face) or "latest" (GitHub).
std::string default_ref(Source s);

struct HubFile {
    std::string path;
    std::optional<std::uint64_t> size;
};
struct Listing {
    std::string ref;  // resolved: GitHub "latest" becomes the release's tag
    std::vector<HubFile> files;
    nlohmann::json to_json() const;  // {"ref", "files": [{"path", "size_bytes"}]}
};
// Parses a hub API reply (HF tree array, or a GitHub release object). Throws std::runtime_error.
Listing parse_listing(Source s, const nlohmann::json& body, const std::string& requested_ref);
// The error message in a hub's JSON error reply ({"error": ...} or {"message": ...}), or "".
std::string hub_error_message(const nlohmann::json& body);

bool is_gguf(std::string_view path);

// A pasted file link: https://huggingface.co/<o>/<r>/(blob|resolve)/<ref>/<path>[?...]
// or https://github.com/<o>/<r>/releases/download/<tag>/<name>.
struct HubRef {
    Source source = Source::huggingface;
    std::string repo, ref, file;
};
std::optional<HubRef> parse_hub_url(std::string_view url);

// "1.5 GB", "820 MB", "12 KB", "512 B" (decimal units, as in the docs).
std::string format_bytes(double bytes);
// "45s", "4m05s", "2h13m".
std::string format_duration(double seconds);

}  // namespace xgi::hub
