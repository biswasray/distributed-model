#include "common/shutdown.hpp"

#include "common/platform.hpp"

#include <atomic>
#include <thread>
#include <utility>

#if defined(XGI_WINDOWS)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace xgi {
namespace {

std::function<void()> g_callback;
std::atomic<bool> g_fired{false};

void fire() {
    if (!g_fired.exchange(true) && g_callback) g_callback();
}

#if defined(XGI_WINDOWS)

BOOL WINAPI console_handler(DWORD) {
    fire();  // runs on a thread the system creates for us
    return TRUE;
}

#else

int g_pipe[2] = {-1, -1};

extern "C" void signal_handler(int) {
    char c = 1;
    ssize_t r = ::write(g_pipe[1], &c, 1);  // async-signal-safe
    (void)r;
}

#endif

}  // namespace

void install_shutdown_handler(std::function<void()> on_shutdown) {
    g_callback = std::move(on_shutdown);
#if defined(XGI_WINDOWS)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGPIPE, SIG_IGN);
    if (::pipe(g_pipe) != 0) return;
    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::thread([] {
        char c;
        while (true) {
            ssize_t n = ::read(g_pipe[0], &c, 1);
            if (n == 1) break;
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
        }
        fire();
    }).detach();
#endif
}

}  // namespace xgi
