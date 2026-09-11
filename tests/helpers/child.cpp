// A tiny stand-in for a solution binary, covering the subprocess behaviours the runner has
// to survive. Deliberately built WITHOUT sanitizers: a sanitized build turns the `crash`
// mode into an ASan report and a plain non-zero exit, which would defeat its own test.
//
// Usage: child <mode> [args...]

#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

void write_all(int fd, const char* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    const ssize_t k = ::write(fd, data + off, n - off);
    if (k <= 0) {
      if (k < 0 && errno == EINTR) continue;
      return;  // EPIPE or similar: the reader is gone, nothing useful left to do
    }
    off += static_cast<size_t>(k);
  }
}

void sleep_ms(long ms) {
  timespec ts{ms / 1000, (ms % 1000) * 1000000};
  while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

long arg_num(int argc, char** argv, int index, long fallback) {
  return index < argc ? ::strtol(argv[index], nullptr, 10) : fallback;
}

int cat_stdin(int dest_fd) {
  std::vector<char> buf(64 * 1024);
  for (;;) {
    const ssize_t k = ::read(STDIN_FILENO, buf.data(), buf.size());
    if (k == 0) return 0;
    if (k < 0) {
      if (errno == EINTR) continue;
      return 1;
    }
    write_all(dest_fd, buf.data(), static_cast<size_t>(k));
  }
}

// Forks a grandchild into the same process group, so it only dies if the runner signals
// the group rather than the child alone. It records its pid so the test can check.
int spawn_grandchild(const char* pidfile, long live_ms, bool hold_stdout) {
  const pid_t kid = ::fork();
  if (kid < 0) return 3;
  if (kid == 0) {
    if (!hold_stdout) {
      const int devnull = ::open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) ::close(devnull);
      }
    }
    const std::string pid = std::to_string(::getpid());
    const std::string tmp = std::string{pidfile} + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      write_all(fd, pid.c_str(), pid.size());
      ::close(fd);
      ::rename(tmp.c_str(), pidfile);  // atomic: the test never sees a half-written pid
    }
    sleep_ms(live_ms);
    ::_exit(0);
  }
  // Wait for the pid to land before exiting, so the test has no race to lose.
  for (int i = 0; i < 2000 && ::access(pidfile, F_OK) != 0; ++i) sleep_ms(1);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    ::fprintf(stderr, "usage: child <mode> [args...]\n");
    return 2;
  }
  const std::string mode = argv[1];

  if (mode == "echo") return cat_stdin(STDOUT_FILENO);
  if (mode == "echo-err") return cat_stdin(STDERR_FILENO);

  if (mode == "hello") {
    const char kOut[] = "out\n";
    const char kErr[] = "err\n";
    write_all(STDOUT_FILENO, kOut, sizeof(kOut) - 1);
    write_all(STDERR_FILENO, kErr, sizeof(kErr) - 1);
    return 0;
  }

  if (mode == "exit") return static_cast<int>(arg_num(argc, argv, 2, 0));

  if (mode == "crash") {
    ::raise(SIGSEGV);
    return 0;  // not reached
  }

  if (mode == "sleep") {
    sleep_ms(arg_num(argc, argv, 2, 10000));
    return 0;
  }

  if (mode == "spin") {  // burn CPU, do not sleep: this is what a TLE looks like
    const long ms = arg_num(argc, argv, 2, 100);
    timespec begin{};
    ::clock_gettime(CLOCK_MONOTONIC, &begin);
    volatile uint64_t sink = 0;
    for (;;) {
      for (int i = 0; i < 100000; ++i) sink = sink + static_cast<uint64_t>(i);
      timespec now{};
      ::clock_gettime(CLOCK_MONOTONIC, &now);
      const long elapsed =
          (now.tv_sec - begin.tv_sec) * 1000 + (now.tv_nsec - begin.tv_nsec) / 1000000;
      if (elapsed >= ms) break;
    }
    return 0;
  }

  if (mode == "flood") {  // write a lot without ever reading stdin
    const long total = arg_num(argc, argv, 2, 1 << 20);
    std::vector<char> buf(64 * 1024, 'x');
    long sent = 0;
    while (sent < total) {
      const size_t n = static_cast<size_t>(total - sent < static_cast<long>(buf.size())
                                               ? total - sent
                                               : static_cast<long>(buf.size()));
      write_all(STDOUT_FILENO, buf.data(), n);
      sent += static_cast<long>(n);
    }
    return 0;
  }

  if (mode == "ignore-stdin") return 0;  // exits with input still pending: SIGPIPE bait

  if (mode == "orphan") return spawn_grandchild(argv[2], arg_num(argc, argv, 3, 60000), false);
  if (mode == "orphan-hold") return spawn_grandchild(argv[2], arg_num(argc, argv, 3, 60000), true);

  ::fprintf(stderr, "child: unknown mode %s\n", mode.c_str());
  return 2;
}
