#include "evaluator.hpp"
#include "runner_backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <sys/wait.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

namespace {
struct Workspace {
    std::filesystem::path directory;
    Workspace() {
        char pattern[] = "/tmp/shrink-execution-XXXXXX";
        const auto path = mkdtemp(pattern);
        if (!path) throw std::runtime_error("temporary directory");
        directory = path;
    }
    ~Workspace() { std::filesystem::remove_all(directory); }
};
// Force initial cleanup expiry without collecting real child statuses. Recovery
// resumes the same native handles; no fabricated PIDs or assumed scheduling.
class DeferredStatusCollection final : public shrink::detail::ExecutionBackend {
    std::unique_ptr<ExecutionBackend> native_ = shrink::detail::make_mac_execution_backend();
    shrink::CancellationSource cancellation_;
    std::uint64_t offset_ = 0;
    bool deferred_ = true;
public:
    explicit DeferredStatusCollection(shrink::CancellationSource cancellation)
        : cancellation_(std::move(cancellation)) {}
    void resume() { deferred_ = false; }
    std::uint64_t now_ms() override { return native_->now_ms() + offset_; }
    std::optional<shrink::RunnerError> launch(shrink::ProgramRole p, const shrink::ProgramSpec& spec, std::string_view input) override {
        auto error = native_->launch(p, spec, input);
        if (!error && p == shrink::ProgramRole::reference) cancellation_.request_stop();
        return error;
    }
    std::vector<shrink::detail::ExecutionEvent> service(shrink::CancellationToken token, int wait_ms) override {
        if (deferred_) { offset_ += 1000; return {}; }
        return native_->service(token, wait_ms);
    }
    std::optional<shrink::RunnerError> stop(shrink::ProgramRole p) override { return native_->stop(p); }
    bool pending(shrink::ProgramRole p) const override { return native_->pending(p); }
    std::optional<int> process_id(shrink::ProgramRole p) const override { return native_->process_id(p); }
    shrink::PendingOwnership ownership(shrink::ProgramRole p) const override { return native_->ownership(p); }
};
shrink::ProgramSpec text(std::string value) {
    return {SHRINK_PROGRAM_FIXTURE, {"text", std::move(value)}};
}
}
TEST_CASE("candidate preserves exact token disagreement with owning evidence", "[runner]") {
    shrink::Runner runner;
    auto result = shrink::evaluate_candidate(runner, "0\n\n", text("1"), text("1.0"), {});
    REQUIRE(result.execution.errors.empty());
    REQUIRE(result.outcome == shrink::CandidateOutcome::preserved);
    REQUIRE(result.disagreement->token_position == 1);
    REQUIRE(result.execution.programs[0].stdout_bytes == "1");
    REQUIRE(result.execution.programs[1].stdout_bytes == "1.0");
    REQUIRE_FALSE(runner.has_pending_ownership());
    auto next = shrink::evaluate_candidate(runner, "0\n\n", text("\t 1\n2 "), text("1 2"), {});
    REQUIRE(next.outcome == shrink::CandidateOutcome::lost);
    REQUIRE(result.execution.programs[1].stdout_bytes == "1.0");
}
TEST_CASE("stdout permits exactly eight MiB and rejects the first additional byte", "[runner]") {
    shrink::Runner runner;
    const auto emit = [](std::string size) { return shrink::ProgramSpec{SHRINK_PROGRAM_FIXTURE, {"emit", size, "0"}}; };
    auto exact = shrink::evaluate_candidate(runner, "0\n\n", emit("8388608"), emit("8388608"), {});
    REQUIRE(exact.outcome == shrink::CandidateOutcome::lost);
    REQUIRE(exact.execution.programs[0].successful());
    REQUIRE(exact.execution.programs[0].stdout_bytes.size() == 8'388'608);
    auto over = shrink::evaluate_candidate(runner, "0\n\n", emit("8388609"), text("answer"), {});
    REQUIRE(over.outcome == shrink::CandidateOutcome::lost);
    REQUIRE_FALSE(over.execution.programs[0].successful());
    REQUIRE(over.execution.programs[0].stdout_bytes.size() == 8'388'608);
    REQUIRE(over.execution.programs[0].stop_reasons == std::vector{shrink::StopReason::stdout_overflow});
}
TEST_CASE("concurrent large input and output drain fairly while stderr retains only its prefix", "[runner]") {
    shrink::Runner runner;
    const shrink::ProgramSpec program{SHRINK_PROGRAM_FIXTURE, {"duplex"}};
    const std::string input = "1048576\n" + std::string(1'048'575, '1') + "\n";
    auto result = shrink::evaluate_candidate(runner, input, program, program, {});
    REQUIRE(result.execution.errors.empty());
    REQUIRE(result.outcome == shrink::CandidateOutcome::lost);
    for (const auto& evidence : result.execution.programs) {
        REQUIRE(evidence.successful());
        REQUIRE(evidence.stdout_bytes == std::string(1'048'576, 'x') + input);
        REQUIRE(evidence.stderr_bytes == std::string(65'536, 'e'));
        REQUIRE(evidence.stderr_truncated);
    }
}
TEST_CASE("completed evaluations expose reaped direct children", "[runner]") {
    shrink::Runner runner;
    auto result = shrink::evaluate_candidate(runner, "0\n\n", text("a"), text("b"), {});
    REQUIRE(result.outcome == shrink::CandidateOutcome::preserved);
    for (const auto& evidence : result.execution.programs) {
        REQUIRE(evidence.process_id.has_value());
        int status = 0;
        errno = 0;
        REQUIRE(waitpid(*evidence.process_id, &status, WNOHANG) == -1);
        REQUIRE(errno == ECHILD);
        REQUIRE(evidence.reaped);
        REQUIRE(evidence.cleanup_complete);
    }
}
TEST_CASE("real exit 127 and signal facts remain distinct from exec failure", "[runner]") {
    shrink::Runner runner;
    auto ordinary = shrink::evaluate_candidate(runner, "0\n\n", text("1"),
        {SHRINK_PROGRAM_FIXTURE, {"exit", "127"}}, {});
    REQUIRE(ordinary.execution.errors.empty());
    REQUIRE(ordinary.outcome == shrink::CandidateOutcome::inconclusive);
    REQUIRE(ordinary.execution.programs[1].exit_code == 127);
    auto missing = shrink::evaluate_candidate(runner, "0\n\n", text("1"),
        {"/nonexistent/shrink-test-program", {}}, {});
    REQUIRE_FALSE(missing.outcome);
    REQUIRE_FALSE(missing.execution.errors.empty());
    REQUIRE(missing.execution.errors[0].code == shrink::RunnerErrorCode::launch);
    REQUIRE(missing.execution.errors[0].system_error == ENOENT);
    REQUIRE(missing.execution.errors[0].program == shrink::ProgramRole::reference);
    auto signalled = shrink::evaluate_candidate(runner, "0\n\n",
        {SHRINK_PROGRAM_FIXTURE, {"signal", std::to_string(SIGTERM)}}, text("1"), {});
    REQUIRE(signalled.outcome == shrink::CandidateOutcome::lost);
    REQUIRE(signalled.execution.errors.empty());
    REQUIRE(signalled.execution.programs[0].signal == SIGTERM);
    REQUIRE(signalled.execution.programs[0].stop_reasons.empty());
}
TEST_CASE("both programs launch before either must finish", "[runner]") {
    Workspace workspace;
    const auto a = (workspace.directory / "solution-ready").string();
    const auto b = (workspace.directory / "reference-ready").string();
    shrink::Runner runner;
    auto result = shrink::evaluate_candidate(runner, "0\n\n",
        {SHRINK_PROGRAM_FIXTURE, {"rendezvous", a, b}},
        {SHRINK_PROGRAM_FIXTURE, {"rendezvous", b, a}}, {});
    REQUIRE(result.execution.errors.empty());
    REQUIRE(result.outcome == shrink::CandidateOutcome::lost);
    REQUIRE(result.execution.programs[0].successful());
    REQUIRE(result.execution.programs[1].successful());
}
TEST_CASE("token cancellation kills and reaps programs that ignore ordinary interrupts", "[runner]") {
    Workspace workspace;
    const auto a = workspace.directory / "solution-ready";
    const auto b = workspace.directory / "reference-ready";
    shrink::CancellationSource source;
    std::atomic<bool> synchronized = false;
    std::thread cancel([&] {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((!std::filesystem::exists(a) || !std::filesystem::exists(b)) && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        synchronized = std::filesystem::exists(a) && std::filesystem::exists(b);
        source.request_stop();
    });
    shrink::Runner runner;
    auto result = shrink::evaluate_candidate(runner, "0\n\n",
        {SHRINK_PROGRAM_FIXTURE, {"wait", a.string()}}, {SHRINK_PROGRAM_FIXTURE, {"wait", b.string()}}, {}, source.token());
    cancel.join();
    REQUIRE(synchronized);
    REQUIRE(result.execution.cancelled);
    REQUIRE(result.execution.errors.empty());
    REQUIRE_FALSE(result.outcome);
    for (const auto& evidence : result.execution.programs) {
        REQUIRE(evidence.signal == SIGKILL);
        REQUIRE(evidence.stop_reasons == std::vector{shrink::StopReason::cancellation});
        REQUIRE(evidence.reaped);
        REQUIRE(evidence.cleanup_complete);
    }
    REQUIRE_FALSE(runner.has_pending_ownership());
}
TEST_CASE("per-program timeout stops its peer and retains observed termination separately", "[runner]") {
    Workspace workspace;
    shrink::RunLimits limits; limits.solution_timeout = std::chrono::milliseconds(500);
    shrink::Runner runner;
    auto result = shrink::evaluate_candidate(runner, "0\n\n",
        {SHRINK_PROGRAM_FIXTURE, {"wait", (workspace.directory / "s").string()}},
        {SHRINK_PROGRAM_FIXTURE, {"wait", (workspace.directory / "r").string()}}, limits);
    REQUIRE(result.execution.errors.empty());
    REQUIRE(result.outcome == shrink::CandidateOutcome::lost);
    REQUIRE(result.execution.programs[0].stop_reasons == std::vector{shrink::StopReason::timeout});
    REQUIRE(result.execution.programs[1].stop_reasons == std::vector{shrink::StopReason::peer_failure});
    REQUIRE(result.execution.programs[0].signal == SIGKILL);
    REQUIRE(result.execution.programs[0].elapsed_ms >= 500);
    REQUIRE(result.execution.programs[0].reaped);
    REQUIRE(result.execution.programs[1].reaped);
}
TEST_CASE("early stdin closure and permission failure release every owned descriptor", "[runner]") {
    Workspace workspace;
    const auto forbidden = workspace.directory / "not executable";
    { std::ofstream file(forbidden); file << "not executable"; }
    const auto descriptors = [] {
        std::vector<int> result;
        for (int fd = 0; fd < getdtablesize(); ++fd) if (fcntl(fd, F_GETFD) >= 0) result.push_back(fd);
        return result;
    };
    const auto before = descriptors();
    shrink::Runner runner;
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto early = shrink::evaluate_candidate(runner, std::string(1'048'576, '1'), text("1"), text("1"), {});
        REQUIRE(early.outcome == shrink::CandidateOutcome::lost);
        REQUIRE(early.execution.errors.empty());
        REQUIRE(early.execution.programs[0].successful());
        auto denied = shrink::evaluate_candidate(runner, "0\n\n", text("1"), {forbidden, {}}, {});
        REQUIRE_FALSE(denied.outcome);
        REQUIRE(denied.execution.errors[0].system_error == EACCES);
        REQUIRE_FALSE(runner.has_pending_ownership());
    }
    REQUIRE(descriptors() == before);
}
TEST_CASE("runner preserves caller signal state in supplied programs", "[runner]") {
    struct SignalState {
        sigset_t previous_mask;
        struct sigaction previous_action{};
        SignalState() {
            sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR2);
            if (sigprocmask(SIG_BLOCK, &mask, &previous_mask) != 0) throw std::runtime_error("signal mask");
            struct sigaction ignored{}; ignored.sa_handler = SIG_IGN; sigemptyset(&ignored.sa_mask);
            if (sigaction(SIGUSR1, &ignored, &previous_action) != 0) throw std::runtime_error("signal disposition");
        }
        ~SignalState() { sigprocmask(SIG_SETMASK, &previous_mask, nullptr); sigaction(SIGUSR1, &previous_action, nullptr); }
    } signals;
    shrink::Runner runner;
    auto result = shrink::evaluate_candidate(runner, "0\n\n",
        {SHRINK_PROGRAM_FIXTURE, {"signal-state"}}, text("1 1"), {});
    REQUIRE(result.execution.errors.empty());
    REQUIRE(result.outcome == shrink::CandidateOutcome::lost);
    REQUIRE(result.execution.programs[0].stdout_bytes == "1 1");
    sigset_t mask; sigprocmask(SIG_SETMASK, nullptr, &mask);
    struct sigaction action{}; sigaction(SIGUSR1, nullptr, &action);
    REQUIRE(sigismember(&mask, SIGUSR2) == 1);
    REQUIRE(action.sa_handler == SIG_IGN);
}
TEST_CASE("token evidence identifies end-of-output and compares all separating whitespace", "[runner]") {
    shrink::Runner runner;
    auto empty = shrink::evaluate_candidate(runner, "0\n\n", text(" \t\r\n\v\f"), text(""), {});
    REQUIRE(empty.outcome == shrink::CandidateOutcome::lost);
    auto unequal = shrink::evaluate_candidate(runner, "0\n\n", text("a b \n"), text("\ta"), {});
    REQUIRE(unequal.outcome == shrink::CandidateOutcome::preserved);
    REQUIRE(unequal.disagreement->token_position == 2);
    REQUIRE(unequal.disagreement->solution.offset == 2);
    REQUIRE(unequal.disagreement->solution.length == 1);
    REQUIRE_FALSE(unequal.disagreement->solution.end_of_output);
    REQUIRE(unequal.disagreement->reference.end_of_output);
    REQUIRE(unequal.disagreement->reference.offset == 2);
    auto reverse = shrink::evaluate_candidate(runner, "0\n\n", text("a"), text("a b"), {});
    REQUIRE(reverse.disagreement->solution.end_of_output);
    REQUIRE(reverse.disagreement->reference.length == 1);
}
TEST_CASE("explicit paths inherit working directory and environment with literal ordered arguments", "[runner]") {
    Workspace workspace;
    const auto executable = workspace.directory / "program with spaces";
    std::filesystem::copy_file(SHRINK_PROGRAM_FIXTURE, executable);
    struct Environment {
        std::optional<std::string> previous;
        Environment() {
            if (const auto value = getenv("SHRINK_TEST_INHERITED")) previous = value;
            setenv("SHRINK_TEST_INHERITED", "inherited value", 1);
        }
        ~Environment() {
            if (previous) setenv("SHRINK_TEST_INHERITED", previous->c_str(), 1);
            else unsetenv("SHRINK_TEST_INHERITED");
        }
    } environment;
    shrink::Runner runner;
    const auto relative = std::filesystem::relative(executable, std::filesystem::current_path());
    auto result = shrink::evaluate_candidate(runner, "0\n\n",
        {relative, {"arguments", "a b", "$(echo shell)", "", "--flag"}}, text("different"), {});
    REQUIRE(result.outcome == shrink::CandidateOutcome::preserved);
    REQUIRE(result.execution.programs[0].stdout_bytes == std::filesystem::current_path().string()
        + "\ninherited value\n3:a b\n13:$(echo shell)\n0:\n6:--flag\n");
    auto no_lookup = shrink::evaluate_candidate(runner, "0\n\n", {"true", {}}, text("1"), {});
    REQUIRE_FALSE(no_lookup.outcome);
    REQUIRE(no_lookup.execution.errors[0].system_error == ENOENT);
}
TEST_CASE("runner evaluation and cleanup never reap a host-owned child", "[runner]") {
    const auto host = fork();
    REQUIRE(host >= 0);
    if (host == 0) _exit(23);
    shrink::CancellationSource cancellation;
    auto backend = std::make_unique<DeferredStatusCollection>(cancellation);
    auto* controlled = backend.get();
    shrink::Runner runner(std::move(backend));
    auto result = shrink::evaluate_candidate(runner, "0\n\n", text("a"), text("b"), {}, cancellation.token());
    const auto pending = runner.pending_ownership();
    controlled->resume();
    auto cleanup = runner.cleanup();
    int status = 0;
    pid_t waited = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        waited = waitpid(host, &status, WNOHANG);
        if (waited != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.errors[0].code == shrink::RunnerErrorCode::cleanup);
    REQUIRE(pending.size() == 2);
    REQUIRE(pending[0].awaiting_reap);
    REQUIRE(pending[1].awaiting_reap);
    REQUIRE(cleanup.resolved);
    REQUIRE(cleanup.errors.empty());
    REQUIRE(cleanup.execution.programs[0].reaped);
    REQUIRE(cleanup.execution.programs[1].reaped);
    REQUIRE_FALSE(runner.has_pending_ownership());
    REQUIRE(waited == host);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 23);
}
TEST_CASE("partial startup failure kills and reaps the already launched peer", "[runner]") {
    Workspace workspace;
    shrink::Runner runner;
    auto result = shrink::evaluate_candidate(runner, "0\n\n",
        {SHRINK_PROGRAM_FIXTURE, {"wait", (workspace.directory / "ready").string()}},
        {workspace.directory / "missing", {}}, {});
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.errors[0].code == shrink::RunnerErrorCode::launch);
    REQUIRE(result.execution.programs[0].launched);
    REQUIRE(result.execution.programs[0].signal == SIGKILL);
    REQUIRE(result.execution.programs[0].reaped);
    REQUIRE(result.execution.programs[1].reaped);
    REQUIRE_FALSE(runner.has_pending_ownership());
}
