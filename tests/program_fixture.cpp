#include <algorithm>
#include <iostream>
#include <string>
#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace {
void emit(std::ostream& stream, std::size_t size, char byte) {
    const std::string block(16384, byte);
    while (size) { const auto count = std::min(size, block.size()); stream.write(block.data(), count); size -= count; }
    stream.flush();
}
}
int main(int argc, char** argv) {
    if (argc < 2) return 99;
    const std::string mode = argv[1];
    if (mode == "text" && argc == 3) std::cout << argv[2];
    else if (mode == "slow-text" && argc == 4) {
        usleep(static_cast<useconds_t>(std::stoul(argv[2]) * 1000));
        std::cout << argv[3];
    }
    else if (mode == "record-input" && argc == 4) {
        std::ofstream record(argv[2], std::ios::binary | std::ios::app);
        record << std::cin.rdbuf();
        if (!record) return 96;
        std::cout << argv[3];
    }
    else if (mode == "fail-stderr" && (argc == 3 || argc == 4)) {
        emit(std::cerr, std::stoull(argv[2]), argc == 4 ? static_cast<char>(std::stoi(argv[3])) : 'e'); return 7;
    }
    else if (mode == "emit" && argc == 4) {
        emit(std::cout, std::stoull(argv[2]), 'x');
        emit(std::cerr, std::stoull(argv[3]), 'e');
    } else if (mode == "duplex") {
        emit(std::cout, 1'048'576, 'x');
        emit(std::cerr, 1'048'576, 'e');
        std::cout << std::cin.rdbuf();
    } else if (mode == "exit" && argc == 3) return std::stoi(argv[2]);
    else if (mode == "signal" && argc == 3) { raise(std::stoi(argv[2])); return 98; }
    else if (mode == "wait" && argc == 3) {
        signal(SIGINT, SIG_IGN);
        { std::ofstream ready(argv[2]); ready << "ready"; }
        for (;;) pause();
    } else if ((mode == "rendezvous" || mode == "rendezvous-ignore-interrupt") && argc == 4) {
        if (mode == "rendezvous-ignore-interrupt") signal(SIGINT, SIG_IGN);
        sigset_t mask; sigprocmask(SIG_SETMASK, nullptr, &mask);
        struct sigaction action{}; sigaction(SIGINT, nullptr, &action);
        { std::ofstream ready(argv[2]);
          ready << getpid() << ' ' << getpgrp() << ' ' << sigismember(&mask, SIGINT)
                << ' ' << (action.sa_handler == SIG_DFL); }
        for (int i = 0; i < 5000 && !std::filesystem::exists(argv[3]); ++i) usleep(1000);
        if (!std::filesystem::exists(argv[3])) return 97;
        std::cout << "met";
    } else if (mode == "arguments") {
        std::cout << std::filesystem::current_path().string() << '\n';
        const auto inherited = std::getenv("SHRINK_TEST_INHERITED");
        std::cout << (inherited ? inherited : "missing") << '\n';
        for (int i = 2; i < argc; ++i) std::cout << std::string(argv[i]).size() << ':' << argv[i] << '\n';
    } else if (mode == "signal-state") {
        sigset_t mask; sigprocmask(SIG_SETMASK, nullptr, &mask);
        struct sigaction action{}; sigaction(SIGUSR1, nullptr, &action);
        std::cout << sigismember(&mask, SIGUSR2) << ' ' << (action.sa_handler == SIG_IGN);
    } else return 99;
}
