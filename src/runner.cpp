#include <shrink/runner.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace shrink {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kChunk = 64 * 1024;  // one pipe buffer's worth
constexpr int kDrainRounds = 64;           // fairness bound per poll wake-up

/// Serializes pipe creation through posix_spawn. Without it, thread A's pipe can be
/// inherited by thread B's child between pipe() and the FD_CLOEXEC fcntl(), and then EOF
/// on A's stdout never arrives because B's child holds the write end open. macOS has no
/// pipe2(O_CLOEXEC), so a lock is the portable fix. Held for microseconds; the child runs
/// outside it.
std::mutex& spawn_mutex() {
  static std::mutex m;
  return m;
}

void close_fd(int& fd) noexcept {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

bool set_flag(int fd, int get, int set, int flag) {
  const int flags = ::fcntl(fd, get);
  if (flags < 0) return false;
  return ::fcntl(fd, set, flags | flag) >= 0;
}

bool set_cloexec(int fd) {
  return set_flag(fd, F_GETFD, F_SETFD, FD_CLOEXEC);
}
bool set_nonblock(int fd) {
  return set_flag(fd, F_GETFL, F_SETFL, O_NONBLOCK);
}

struct Pipe {
  int r{-1};
  int w{-1};

  Pipe() = default;
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;
  ~Pipe() {
    close_fd(r);
    close_fd(w);
  }

  bool open() {
    std::array<int, 2> fds{-1, -1};
    if (::pipe(fds.data()) != 0) return false;
    r = fds[0];
    w = fds[1];
    return set_cloexec(r) && set_cloexec(w);
  }
};

std::chrono::microseconds to_micros(const timeval& tv) {
  return std::chrono::seconds{tv.tv_sec} + std::chrono::microseconds{tv.tv_usec};
}

}  // namespace

void install_sigpipe_guard() noexcept {
  static std::once_flag once;
  std::call_once(once, [] { ::signal(SIGPIPE, SIG_IGN); });
}

std::string_view to_string(RunResult::Status status) noexcept {
  switch (status) {
    case RunResult::Status::Ok:
      return "ok";
    case RunResult::Status::NonZeroExit:
      return "non-zero exit";
    case RunResult::Status::Signaled:
      return "signaled";
    case RunResult::Status::Timeout:
      return "timeout";
    case RunResult::Status::SpawnFailed:
      return "spawn failed";
  }
  return "unknown";
}

std::string describe(const RunResult& result) {
  switch (result.status) {
    case RunResult::Status::Ok:
      return "exit 0";
    case RunResult::Status::NonZeroExit:
      return "exit " + std::to_string(result.exit_code);
    case RunResult::Status::Signaled: {
      const char* name = ::strsignal(result.term_signal);
      return "signal " + std::to_string(result.term_signal) + " (" + (name ? name : "?") + ")";
    }
    case RunResult::Status::Timeout:
      return "timeout";
    case RunResult::Status::SpawnFailed:
      return std::string{"spawn failed: "} + ::strerror(result.spawn_errno);
  }
  return "unknown";
}

RunResult run(const std::vector<std::string>& argv, std::string_view stdin_data,
              const Limits& limits) {
  install_sigpipe_guard();

  RunResult res;
  if (argv.empty()) {
    res.spawn_errno = EINVAL;
    return res;
  }

  const auto start = Clock::now();
  const auto deadline = start + limits.wall;

  Pipe in;
  Pipe out;
  Pipe err;
  pid_t pid = -1;

  {
    const std::lock_guard<std::mutex> lock(spawn_mutex());

    if (!in.open() || !out.open() || !err.open()) {
      res.spawn_errno = errno;
      return res;
    }

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
      res.spawn_errno = errno;
      return res;
    }
    ::posix_spawn_file_actions_adddup2(&actions, in.r, STDIN_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, out.w, STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, err.w, STDERR_FILENO);

    posix_spawnattr_t attr;
    if (::posix_spawnattr_init(&attr) != 0) {
      ::posix_spawn_file_actions_destroy(&actions);
      res.spawn_errno = errno;
      return res;
    }
    // Own process group, so a timeout can kill grandchildren too via kill(-pid).
    ::posix_spawnattr_setpgroup(&attr, 0);
    // An ignored SIGPIPE is inherited across exec. The child must get the default back,
    // or a solution piping into a helper behaves differently under us than on the judge.
    sigset_t defaults;
    sigfillset(&defaults);  // a macro on macOS, so no :: qualifier
    ::posix_spawnattr_setsigdefault(&attr, &defaults);
    sigset_t mask;
    sigemptyset(&mask);
    ::posix_spawnattr_setsigmask(&attr, &mask);
    ::posix_spawnattr_setflags(
        &attr,
        static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    const int rc = ::posix_spawnp(&pid, cargv[0], &actions, &attr, cargv.data(), environ);

    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attr);

    if (rc != 0) {
      res.status = RunResult::Status::SpawnFailed;
      res.spawn_errno = rc;
      return res;
    }
  }

  // Drop the parent's copies of the child's ends, or EOF never arrives on stdout.
  close_fd(in.r);
  close_fd(out.w);
  close_fd(err.w);

  set_nonblock(in.w);
  set_nonblock(out.r);
  set_nonblock(err.r);

  if (stdin_data.empty()) close_fd(in.w);  // EOF immediately, or `while (cin >> x)` spins

  std::size_t written = 0;
  bool timed_out = false;

  auto drain = [&](int& fd, std::string& sink, bool& truncated) {
    std::array<char, kChunk> buf{};
    for (int round = 0; round < kDrainRounds && fd >= 0; ++round) {
      const ssize_t k = ::read(fd, buf.data(), buf.size());
      if (k > 0) {
        const auto n = static_cast<std::size_t>(k);
        if (sink.size() >= limits.max_output) {
          truncated = true;  // keep draining, just stop keeping it
          continue;
        }
        const std::size_t take = std::min(limits.max_output - sink.size(), n);
        sink.append(buf.data(), take);
        if (take < n) truncated = true;
        continue;
      }
      if (k == 0) {
        close_fd(fd);
        return;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      close_fd(fd);
      return;
    }
  };

  // One poll over all three fds at once. Writing stdin to completion before reading stdout
  // deadlocks the moment the child's 64 KB stdout buffer fills.
  while (in.w >= 0 || out.r >= 0 || err.r >= 0) {
    const auto now = Clock::now();
    if (now >= deadline) {
      timed_out = true;
      break;
    }
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1;
    const int timeout_ms = static_cast<int>(std::min<long long>(left, 1000));

    std::array<pollfd, 3> fds{};
    int n = 0;
    int i_in = -1;
    int i_out = -1;
    int i_err = -1;
    if (in.w >= 0) {
      fds[static_cast<std::size_t>(n)] = {in.w, POLLOUT, 0};
      i_in = n++;
    }
    if (out.r >= 0) {
      fds[static_cast<std::size_t>(n)] = {out.r, POLLIN, 0};
      i_out = n++;
    }
    if (err.r >= 0) {
      fds[static_cast<std::size_t>(n)] = {err.r, POLLIN, 0};
      i_err = n++;
    }

    const int pr = ::poll(fds.data(), static_cast<nfds_t>(n), timeout_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;  // unexpected; fall through to reaping rather than spinning
    }
    if (pr == 0) continue;  // re-check the deadline

    if (i_in >= 0) {
      const short ev = fds[static_cast<std::size_t>(i_in)].revents;
      if ((ev & POLLOUT) != 0) {
        const std::size_t remaining = stdin_data.size() - written;
        const ssize_t k = ::write(in.w, stdin_data.data() + written, std::min(remaining, kChunk));
        if (k > 0) {
          written += static_cast<std::size_t>(k);
          if (written == stdin_data.size()) close_fd(in.w);  // EOF for the child
        } else if (k < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
          close_fd(in.w);  // EPIPE: the child stopped reading. Its business, not an error.
        }
      } else if ((ev & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        close_fd(in.w);
      }
    }
    // POLLHUP with data still buffered is normal, so read until read() itself reports EOF.
    if (i_out >= 0 && fds[static_cast<std::size_t>(i_out)].revents != 0) {
      drain(out.r, res.out, res.out_truncated);
    }
    if (i_err >= 0 && fds[static_cast<std::size_t>(i_err)].revents != 0) {
      drain(err.r, res.err, res.err_truncated);
    }
  }

  close_fd(in.w);
  close_fd(out.r);
  close_fd(err.r);

  // Reap without blocking past the deadline: a child can close its pipes and keep running.
  int status = 0;
  rusage usage{};
  bool killed = false;
  if (timed_out) {
    ::kill(-pid, SIGKILL);  // negative pid: the whole group, so grandchildren die too
    ::kill(pid, SIGKILL);   // belt and braces if setpgroup did not take
    killed = true;
  }
  for (;;) {
    const pid_t w = ::wait4(pid, &status, killed ? 0 : WNOHANG, &usage);
    if (w == pid) break;
    if (w < 0) {
      if (errno == EINTR) continue;
      res.status = RunResult::Status::SpawnFailed;  // unreachable barring a stolen child
      res.spawn_errno = errno;
      return res;
    }
    if (Clock::now() >= deadline) {
      timed_out = true;
      ::kill(-pid, SIGKILL);
      ::kill(pid, SIGKILL);
      killed = true;
      continue;
    }
    ::usleep(200);
  }

  res.wall = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
  res.cpu = to_micros(usage.ru_utime) + to_micros(usage.ru_stime);

  if (timed_out) {
    res.status = RunResult::Status::Timeout;
    res.term_signal = SIGKILL;
  } else if (WIFEXITED(status)) {
    res.exit_code = WEXITSTATUS(status);
    res.status = res.exit_code == 0 ? RunResult::Status::Ok : RunResult::Status::NonZeroExit;
  } else if (WIFSIGNALED(status)) {
    res.term_signal = WTERMSIG(status);
    res.status = RunResult::Status::Signaled;
  } else {
    res.status = RunResult::Status::SpawnFailed;
    res.spawn_errno = ECHILD;
  }
  return res;
}

}  // namespace shrink
