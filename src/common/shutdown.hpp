// Ctrl+C / console close (Windows) and SIGINT / SIGTERM (POSIX) -> one callback.
#pragma once

#include <functional>

namespace xgi {

// Calls `on_shutdown` once, on a normal thread (not in signal context), when the process is
// asked to stop. On POSIX this also ignores SIGPIPE, so a client hanging up can't kill the process.
void install_shutdown_handler(std::function<void()> on_shutdown);

}  // namespace xgi
