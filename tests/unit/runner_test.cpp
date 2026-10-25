#include <shrink/runner.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using shrink::Limits;
using shrink::run;
using shrink::RunResult;
using Status = RunResult::Status;

namespace {

const std::string kChild = SHRINK_TEST_CHILD;

std::vector<std::string> child(std::vector<std::string> args) {
  args.insert(args.begin(), kChild);
  return args;
}

/// Deterministic filler, so a failure is reproducible and a diff is meaningful.
///
/// Sized once and written by index, never grown with push_back. GCC's AddressSanitizer on
/// Linux enables stack-use-after-return detection by default, which makes each push_back
/// at -O0 cost microseconds: 10 MB took 193 s that way against 0.2 s written by index. The
/// Mac's ASan leaves that check off, so only the Linux dev build ever showed it.
std::string blob(std::size_t bytes) {
  std::string s(bytes, '\0');
  std::mt19937 rng{12345};
  for (char& c : s) c = static_cast<char>('a' + rng() % 26);
  return s;
}

std::filesystem::path temp_path(const std::string& stem) {
  return std::filesystem::temp_directory_path() /
         (stem + "." + std::to_string(::getpid()) + ".pid");
}

bool alive(pid_t pid) {
  return ::kill(pid, 0) == 0;
}

bool wait_until_gone(pid_t pid, std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!alive(pid)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return !alive(pid);
}

}  // namespace

// The regression test the plan builds the whole module around: 10 MB is 160 pipe buffers,
// so an implementation that writes stdin to completion before reading stdout hangs here.
TEST_CASE("10 MB round-trips through cat without deadlocking", "[runner][deadlock]") {
  const std::string data = blob(10u * 1024 * 1024);
  const auto r = run({"cat"}, data, Limits{.wall = std::chrono::seconds{30}});

  REQUIRE(r.status == Status::Ok);
  REQUIRE(r.out.size() == data.size());
  REQUIRE(r.out == data);
  REQUIRE(r.err.empty());
  REQUIRE_FALSE(r.out_truncated);
}

TEST_CASE("10 MB round-trips through a child that echoes", "[runner][deadlock]") {
  const std::string data = blob(10u * 1024 * 1024);
  const auto r = run(child({"echo"}), data, Limits{.wall = std::chrono::seconds{30}});

  REQUIRE(r.status == Status::Ok);
  REQUIRE(r.out == data);
}

// The mirror image: a child that floods stdout while never reading its stdin. Its stdin
// pipe fills and stays full, so a runner that insists on finishing the write first stalls.
TEST_CASE("a child that floods stdout and ignores stdin does not deadlock", "[runner][deadlock]") {
  const auto r = run(child({"flood", "4194304"}), blob(4u * 1024 * 1024),
                     Limits{.wall = std::chrono::seconds{30}});

  REQUIRE(r.status == Status::Ok);
  REQUIRE(r.out.size() == 4u * 1024 * 1024);
}

TEST_CASE("a sleeping child is killed at the wall limit", "[runner][timeout]") {
  const auto r = run(child({"sleep", "10000"}), "", Limits{.wall = std::chrono::milliseconds{300}});

  REQUIRE(r.status == Status::Timeout);
  REQUIRE(r.term_signal == SIGKILL);
  REQUIRE(r.wall >= std::chrono::milliseconds{300});
  REQUIRE(r.wall < std::chrono::seconds{5});
  // Sleeping burns no CPU. This is exactly why TLE is judged on CPU time, not wall time.
  REQUIRE(r.cpu < std::chrono::milliseconds{200});
}

TEST_CASE("a fatal signal is decoded, not mistaken for an exit code", "[runner][signal]") {
  const auto r = run(child({"crash"}), "");

  REQUIRE(r.status == Status::Signaled);
  REQUIRE(r.term_signal == SIGSEGV);
  REQUIRE(r.exit_code == 0);
  REQUIRE(r.runtime_error());
  REQUIRE(shrink::describe(r).starts_with("signal 11"));
}

TEST_CASE("a non-zero exit is distinct from a signal", "[runner][signal]") {
  const auto r = run(child({"exit", "3"}), "");

  REQUIRE(r.status == Status::NonZeroExit);
  REQUIRE(r.exit_code == 3);
  REQUIRE(r.term_signal == 0);
  REQUIRE(r.runtime_error());
}

TEST_CASE("stdout and stderr are captured separately", "[runner]") {
  const auto r = run(child({"hello"}), "");

  REQUIRE(r.status == Status::Ok);
  REQUIRE(r.out == "out\n");
  REQUIRE(r.err == "err\n");
}

// SIGPIPE's default action terminates the *shrinker*, not the child. If the guard is
// missing this test does not fail, it takes the whole test binary down with it.
TEST_CASE("writing to a child that exited early does not kill us", "[runner][sigpipe]") {
  const std::string data = blob(8u * 1024 * 1024);
  for (int i = 0; i < 5; ++i) {
    const auto r = run(child({"ignore-stdin"}), data);
    REQUIRE(r.status == Status::Ok);
  }
  SUCCEED("survived writing to a closed pipe");
}

TEST_CASE("captured output is capped without stalling the child", "[runner][limits]") {
  const auto r = run(child({"flood", "1048576"}), "",
                     Limits{.wall = std::chrono::seconds{30}, .max_output = 1000});

  REQUIRE(r.status == Status::Ok);  // the child still ran to completion
  REQUIRE(r.out.size() == 1000);
  REQUIRE(r.out_truncated);
}

TEST_CASE("a missing binary is a value, not an exception", "[runner][spawn]") {
  const auto r = run({"./definitely-not-a-real-binary-9f3a"}, "");

  REQUIRE(r.status == Status::SpawnFailed);
  REQUIRE(r.spawn_errno == ENOENT);
  REQUIRE(shrink::describe(r).starts_with("spawn failed"));
}

TEST_CASE("an empty argv is rejected", "[runner][spawn]") {
  const auto r = run({}, "");
  REQUIRE(r.status == Status::SpawnFailed);
}

TEST_CASE("CPU time is measured for a busy child", "[runner][cpu]") {
  const auto r = run(child({"spin", "200"}), "", Limits{.wall = std::chrono::seconds{30}});

  REQUIRE(r.status == Status::Ok);
  REQUIRE(r.cpu >= std::chrono::milliseconds{100});
  REQUIRE(r.wall >= std::chrono::milliseconds{150});
}

// kill(pid) spares grandchildren; kill(-pid) does not. An orphan left spinning skews every
// later timing measurement, which is the quiet way this tool would start lying.
TEST_CASE("a timeout kills grandchildren too", "[runner][pgroup]") {
  const auto pidfile = temp_path("shrink_orphan_hold");
  std::filesystem::remove(pidfile);

  const auto r = run(child({"orphan-hold", pidfile.string(), "60000"}), "",
                     Limits{.wall = std::chrono::milliseconds{500}});

  // The grandchild holds the stdout pipe open, so EOF never arrives and we hit the wall.
  REQUIRE(r.status == Status::Timeout);

  REQUIRE(std::filesystem::exists(pidfile));
  std::ifstream in{pidfile};
  pid_t grandchild = 0;
  in >> grandchild;
  REQUIRE(grandchild > 0);

  REQUIRE(wait_until_gone(grandchild, std::chrono::seconds{2}));
  std::filesystem::remove(pidfile);
}

TEST_CASE("a child that forks and exits is reaped promptly", "[runner][pgroup]") {
  const auto pidfile = temp_path("shrink_orphan");
  std::filesystem::remove(pidfile);

  const auto r =
      run(child({"orphan", pidfile.string(), "400"}), "", Limits{.wall = std::chrono::seconds{10}});

  // The grandchild redirected its std fds, so the pipes close when the child exits and we
  // return immediately rather than waiting out the grandchild's lifetime.
  REQUIRE(r.status == Status::Ok);
  REQUIRE(r.wall < std::chrono::seconds{5});
  std::filesystem::remove(pidfile);
}

TEST_CASE("many runs leave no zombies behind", "[runner][reap]") {
  for (int i = 0; i < 100; ++i) {
    const auto r = run(child({"hello"}), "");
    REQUIRE(r.status == Status::Ok);
  }
  int status = 0;
  const pid_t leftover = ::waitpid(-1, &status, WNOHANG);
  REQUIRE(leftover <= 0);  // a positive pid here would be an unreaped child
}

// run() is called from a thread pool at M6. The hazard is fd inheritance: if one thread's
// pipe leaks into another thread's child, the write end stays open, EOF never arrives, and
// the victim run hangs until its wall limit. Mismatched output or a Timeout here means the
// FD_CLOEXEC window around posix_spawn is unguarded.
TEST_CASE("concurrent runs do not leak pipes into each other", "[runner][threads]") {
  constexpr int kThreads = 8;
  constexpr int kRounds = 6;

  std::vector<std::thread> workers;
  std::vector<std::string> results(kThreads * kRounds);
  std::vector<Status> statuses(kThreads * kRounds, Status::SpawnFailed);

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t] {
      for (int round = 0; round < kRounds; ++round) {
        const int slot = t * kRounds + round;
        const std::string payload = std::to_string(slot) + ":" + blob(200u * 1024);
        const auto r = run(child({"echo"}), payload, Limits{.wall = std::chrono::seconds{20}});
        statuses[static_cast<std::size_t>(slot)] = r.status;
        results[static_cast<std::size_t>(slot)] = r.out == payload ? "match" : "MISMATCH";
      }
    });
  }
  for (auto& w : workers) w.join();

  for (std::size_t i = 0; i < results.size(); ++i) {
    INFO("slot " << i);
    REQUIRE(statuses[i] == Status::Ok);
    REQUIRE(results[i] == "match");
  }
}
