// `xgi download ...`: drive model downloads on the nodes through the coordinator's console API
// (architecture/03-coordinator.md §11, ADR-011).
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace xgi {

// argv as main got it, with argv[1] == "download". Returns the process exit code.
int run_download_cli(int argc, char** argv);

// One progress line for a job as GET /console/downloads returns it, e.g.
// "node-a  [#########...........]  45.2%  1.20 GB / 2.65 GB  5.3 MB/s  ETA 4m31s  file 1/2".
std::string progress_line(const nlohmann::json& job, int bar_width);

}  // namespace xgi
