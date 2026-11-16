// XI's web page files (src/xi/ui/ plus the shared stylesheet src/xgi/ui/app.css), embedded at
// build time by cmake/EmbedAssets.cmake, so xi stays a single file to copy.
#pragma once

#include <cstddef>
#include <string_view>

namespace xi::ui {

struct Asset {
    const char* path;  // e.g. "index.html"
    const unsigned char* data;
    std::size_t size;
};

// nullptr if there is no such file.
const Asset* find_asset(std::string_view path);

}  // namespace xi::ui
