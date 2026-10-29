#include "cli.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <ostream>
#include <unistd.h>

namespace shrink::cli {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt_handler(int) { interrupted = 1; }
}
int run_terminal(std::span<const std::string_view> arguments, std::ostream& output,
                 std::ostream& errors, InvocationServices services) {
    interrupted = 0;
    struct sigaction action{}, previous{};
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &previous) < 0) {
        errors << "Cannot install Ctrl-C handler: " << std::error_code(errno, std::generic_category()).message() << '\n';
        return 1;
    }
    // A broken recovery pipe is an I/O failure. Do not change the signal
    // dispositions that supplied programs inherit.
    (void)fcntl(STDOUT_FILENO, F_SETNOSIGPIPE, 1);
    services.cancellation = services.cancellation.with_stop_check([] { return interrupted != 0; });
    const int status = run(arguments, output, errors, services);
    if (sigaction(SIGINT, &previous, nullptr) < 0) {
        errors << "Cannot restore Ctrl-C handler: " << std::error_code(errno, std::generic_category()).message() << '\n';
        return 1;
    }
    return status;
}
}
