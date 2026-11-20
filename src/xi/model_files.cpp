#include "xi/model_files.hpp"

#include <format>
#include <fstream>
#include <vector>

#include "common/http_util.hpp"
#include "common/hub.hpp"
#include "common/log.hpp"
#include "common/platform.hpp"
#include "common/util.hpp"
#include "xi/commands.hpp"

namespace xi {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr size_t kChunk = 1 << 20;  // 1 MiB per read

}  // namespace

json model_files_json(const fs::path& models_dir) {
    json files = json::array();
    const json sizes = scan_model_sizes(models_dir);  // keep it alive while iterating (items() is a view)
    for (auto& [path, size] : sizes.items()) files.push_back({{"path", path}, {"size_bytes", size}});
    return json{{"files", files}};
}

ModelServer::ModelServer(std::function<fs::path()> models_dir, size_t max_uploads)
    : models_dir_(std::move(models_dir)), max_uploads_(max_uploads) {}

void ModelServer::register_routes(httplib::Server& s) {
    s.Get("/models", [this](const httplib::Request&, httplib::Response& res) {
        xgi::reply_json(res, 200, model_files_json(models_dir_()));
    });

    s.Get(R"(/models/file/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string rel = req.matches[1];  // httplib has already percent-decoded req.path
        if (!xgi::hub::valid_file_path(rel, true) || !xgi::hub::is_gguf(rel))
            return xgi::reply_error(res, 400, "not a .gguf path inside the models folder", "invalid_request");
        auto file = resolve_model_path(models_dir_(), rel);  // inside the folder, after symlinks, regular file
        if (!file) return xgi::reply_error(res, 404, "no model file " + rel + " here", "not_found");

        auto state = state_;
        if (state->active.fetch_add(1) >= max_uploads_) {
            state->active.fetch_sub(1);
            res.set_header("Retry-After", std::to_string(kRetryAfterS));
            return xgi::reply_error(res, 429,
                                    std::format("already sending {} model files; try again shortly", max_uploads_),
                                    "capacity");
        }
        auto in = std::make_shared<std::ifstream>(*file, std::ios::binary);
        std::error_code ec;
        const auto size = static_cast<std::uint64_t>(fs::file_size(*file, ec));
        if (!*in || ec) {
            state->active.fetch_sub(1);
            return xgi::reply_error(res, 500, "cannot read " + rel, "io");
        }
        const std::string who = req.remote_addr, range = req.get_header_value("Range");
        xgi::log::info("models: sending {} ({}{}) to {}", rel, xgi::hub::format_bytes(static_cast<double>(size)),
                       range.empty() ? "" : ", " + range, who);
        if (size == 0) {
            state->active.fetch_sub(1);
            return res.set_content("", "application/octet-stream");
        }
        auto sent = std::make_shared<std::uint64_t>(0);
        auto buf = std::make_shared<std::vector<char>>(kChunk);
        res.set_content_provider(
            static_cast<size_t>(size), "application/octet-stream",
            [in, sent, buf](size_t offset, size_t length, httplib::DataSink& sink) {
                in->clear();
                in->seekg(static_cast<std::streamoff>(offset));
                const size_t n = std::min(length, buf->size());
                in->read(buf->data(), static_cast<std::streamsize>(n));
                const auto got = static_cast<size_t>(in->gcount());
                if (got == 0) return false;  // the file shrank underneath us
                *sent += got;
                return sink.write(buf->data(), got);
            },
            [state, sent, rel, who](bool ok) {
                state->active.fetch_sub(1);
                xgi::log::info("models: {} {} to {} ({} sent)", ok ? "sent" : "stopped sending", rel, who,
                               xgi::hub::format_bytes(static_cast<double>(*sent)));
            });
    });
}

}  // namespace xi
