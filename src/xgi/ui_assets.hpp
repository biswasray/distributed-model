// The console's static files (src/xgi/ui/), embedded at build time by cmake/EmbedAssets.cmake,
// so xgi stays a single file to copy.
#pragma once

#include <cstddef>
#include <string_view>

namespace xgi::ui {

struct Asset {
    const char* path;  // e.g. "index.html"
    const unsigned char* data;
    std::size_t size;
};

// nullptr if there is no such file.
const Asset* find_asset(std::string_view path);

}  // namespace xgi::ui
