#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace shrink {

/// Resource limits for a single child process.
struct Limits {
  /// Hard wall-clock backstop. On expiry the child's whole process group is SIGKILLed.
  /// This is not the TLE metric; that is CPU time, which stays stable under parallel load.
  std::chrono::milliseconds wall{std::chrono::seconds{5}};

  /// Per-stream cap on captured bytes. Beyond it output is drained and discarded, so a
  /// runaway child can neither OOM us nor deadlock on a full pipe.
  std::size_t max_output{64u * 1024u * 1024u};
};

/// Outcome of one child process. Expected failures (crash, timeout, bad spawn) are data,
/// never exceptions; only programmer errors abort.
struct RunResult {
  enum class Status {
    Ok,           ///< exited with code 0
    NonZeroExit,  ///< exited with a non-zero code
    Signaled,     ///< killed by a signal the child did not survive (SIGSEGV, SIGABRT, ...)
    Timeout,      ///< exceeded Limits::wall and was killed by us
    SpawnFailed,  ///< binary missing, not executable, or pipes unavailable
  };

  Status status{Status::SpawnFailed};
  int exit_code{0};    ///< valid when status is Ok or NonZeroExit
  int term_signal{0};  ///< valid when status is Signaled or Timeout
  int spawn_errno{0};  ///< valid when status is SpawnFailed

  std::string out;
  std::string err;
  bool out_truncated{false};
  bool err_truncated{false};

  std::chrono::microseconds wall{0};
  /// ru_utime + ru_stime of the child, from wait4. The metric --mode tle judges on.
  std::chrono::microseconds cpu{0};

  [[nodiscard]] bool ok() const noexcept { return status == Status::Ok; }

  /// A competitive judge calls both a non-zero exit and a fatal signal a runtime error.
  [[nodiscard]] bool runtime_error() const noexcept {
    return status == Status::NonZeroExit || status == Status::Signaled;
  }
};

/// Ignore SIGPIPE process-wide, so writing to a child that already exited returns EPIPE
/// instead of killing the shrinker. Idempotent and thread-safe; run() calls it too, so the
/// guard cannot be forgotten.
void install_sigpipe_guard() noexcept;

/// Run argv with stdin_data on its standard input, capturing stdout and stderr.
///
/// argv[0] is resolved through PATH when it contains no slash. No shell is involved, so
/// arguments never undergo word splitting or globbing.
[[nodiscard]] RunResult run(const std::vector<std::string>& argv, std::string_view stdin_data,
                            const Limits& limits = {});

[[nodiscard]] std::string_view to_string(RunResult::Status status) noexcept;

/// Human-readable one-liner, e.g. "signal 11 (SIGSEGV)" or "exit 3".
[[nodiscard]] std::string describe(const RunResult& result);

}  // namespace shrink
