#include "cli.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt_handler(int) { interrupted = 1; }
}
int main(int argc, char* argv[]) {
    std::vector<std::string_view> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    struct sigaction action{};
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) < 0) {
        std::cerr << "Cannot install Ctrl-C handler: " << std::error_code(errno, std::generic_category()).message() << '\n';
        return 1;
    }
    // A broken recovery pipe must be reported as an I/O failure, without
    // changing signal dispositions inherited by the supplied programs.
    (void)fcntl(STDOUT_FILENO, F_SETNOSIGPIPE, 1);
    shrink::cli::InvocationServices services;
    services.cancellation = shrink::CancellationToken{}.with_stop_check([] { return interrupted != 0; });
    return shrink::cli::run(arguments, std::cout, std::cerr, services);
}
