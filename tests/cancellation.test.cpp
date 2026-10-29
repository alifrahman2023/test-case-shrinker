#include "cli.hpp"
#include "saving.hpp"
#include "runner_backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <csignal>
#include <fstream>
#include <sstream>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace shrink;
namespace fs = std::filesystem;
struct Workspace {
    fs::path directory;
    Workspace() {
        char pattern[] = "/tmp/shrink-cancellation-XXXXXX";
        const auto created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("cannot create cancellation workspace");
        directory = created;
        { std::ofstream input(directory / "input"); input << " 3\t7 -2 7 "; }
        { std::ofstream output(directory / "result"); output << "previous\n"; }
    }
    ~Workspace() { fs::remove_all(directory); }
    std::string read(const char* name) const {
        std::ifstream file(directory / name, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), {}};
    }
    std::vector<std::string> arguments() const {
        return {"--input=" + (directory / "input").string(),
                "--output=" + (directory / "result").string(),
                "--solution=" SHRINK_PROGRAM_FIXTURE, "--reference=" SHRINK_PROGRAM_FIXTURE,
                "--solution-arg=record-input", "--solution-arg=" + (directory / "solution-input").string(), "--solution-arg=1",
                "--reference-arg=record-input", "--reference-arg=" + (directory / "reference-input").string(), "--reference-arg=2",
                "--min-length=0", "--max-length=100", "--min-value=-100", "--max-value=100", "--max-evaluations=0"};
    }
};
struct Child {
    pid_t pid;
    bool reaped = false;
    bool wait(int& status) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            const auto result = waitpid(pid, &status, WNOHANG);
            if (result == pid) { reaped = true; return true; }
            if (result < 0 && errno != EINTR) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < end);
        return false;
    }
    ~Child() {
        if (pid > 0 && !reaped) { kill(pid, SIGKILL); int status; (void)wait(status); }
    }
};
bool ready(int descriptor) {
    pollfd event{descriptor, POLLIN, 0};
    int result;
    do { result = poll(&event, 1, 3000); } while (result < 0 && errno == EINTR);
    char byte;
    return result > 0 && read(descriptor, &byte, 1) == 1;
}
}
TEST_CASE("terminal Ctrl-C during saving preserves the verified baseline and attempts final saving", "[cli-cancellation]") {
    struct Case { const char* name; SaveKind phase; bool fail_initial, fail_final, fail_stdout; };
    for (const auto& test : std::vector<Case>{
        {"initial save", SaveKind::initial, false, false, false},
        {"final save", SaveKind::final, false, false, false},
        {"initial failure with successful final retry", SaveKind::initial, true, false, false},
        {"final failure with stdout recovery", SaveKind::final, false, true, false},
        {"both saves fail", SaveKind::initial, true, true, false},
        {"final failure and failed stdout recovery", SaveKind::final, false, true, true}}) {
        INFO(test.name);
        Workspace workspace;
        int reached[2], resume[2];
        REQUIRE(pipe(reached) == 0);
        REQUIRE(pipe(resume) == 0);
        Child child{fork()};
        REQUIRE(child.pid >= 0);
        if (child.pid == 0) {
            close(reached[0]); close(resume[1]);
            auto arguments = workspace.arguments();
            std::vector<std::string_view> views(arguments.begin(), arguments.end());
            std::ofstream output(workspace.directory / "stdout"), errors(workspace.directory / "stderr");
            if (test.fail_stdout) output.setstate(std::ios::badbit);
            cli::InvocationServices services;
            services.save = [&](const fs::path& path, const SaveRequest& save) -> SaveResult {
                { std::ofstream calls(workspace.directory / "saves", std::ios::app);
                  calls << (save.kind == SaveKind::initial ? "initial\n" : "final\n"); }
                if (save.kind == test.phase) {
                    if (write(reached[1], "r", 1) != 1) _exit(91);
                    char byte;
                    ssize_t count;
                    do { count = read(resume[0], &byte, 1); } while (count < 0 && errno == EINTR);
                    if (count != 1) _exit(92);
                }
                if ((test.fail_initial && save.kind == SaveKind::initial) ||
                    (test.fail_final && save.kind == SaveKind::final))
                    return SaveError{SaveErrorCode::replace, "scripted replacement failure", path,
                                     std::make_error_code(std::errc::permission_denied)};
                return cli::save_atomic(path, save);
            };
            const auto status = cli::run_terminal(views, output, errors, services);
            output.flush(); errors.flush();
            _exit(status);
        }
        close(reached[1]); close(resume[0]);
        const bool synchronized = ready(reached[0]);
        const int signalled = synchronized ? kill(child.pid, SIGINT) : -1;
        const auto released = synchronized ? write(resume[1], "r", 1) : -1;
        close(reached[0]); close(resume[1]);
        int status = 0;
        const bool exited = child.wait(status);
        REQUIRE(synchronized);
        REQUIRE(signalled == 0);
        REQUIRE(released == 1);
        REQUIRE(exited);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == (test.fail_initial || test.fail_final ? 1 : 130));
        REQUIRE(workspace.read("result") == (test.fail_initial && test.fail_final ? "previous\n" : "3\n7 -2 7\n"));
        REQUIRE(workspace.read("saves") == "initial\nfinal\n");
        REQUIRE(workspace.read("solution-input") == "3\n7 -2 7\n");
        REQUIRE(workspace.read("reference-input") == "3\n7 -2 7\n");
        REQUIRE(workspace.read("stdout") == (test.fail_final && !test.fail_stdout ? "3\n7 -2 7\n" : ""));
        REQUIRE(workspace.read("stderr").find("caller cancellation") != std::string::npos);
        REQUIRE(workspace.read("stderr").find(test.fail_final ? "Final save: failed" : "Final save: succeeded") != std::string::npos);
        if (test.fail_initial || test.fail_final) REQUIRE(workspace.read("stderr").find("scripted replacement failure") != std::string::npos);
        if (test.fail_stdout) REQUIRE(workspace.read("stderr").find("stdout recovery failed") != std::string::npos);
        REQUIRE(workspace.read("stderr").find("No reduction round began") != std::string::npos);
    }
}
TEST_CASE("foreground Ctrl-C is isolated from supplied programs and unverified output", "[cli-cancellation]") {
    for (bool ignore_interrupt : {false, true}) {
        INFO("programs ignore SIGINT: " << ignore_interrupt);
        Workspace workspace;
        auto arguments = workspace.arguments();
        const auto solution_ready = workspace.directory / "solution-ready";
        const auto reference_ready = workspace.directory / "reference-ready";
        const auto missing = workspace.directory / "missing-peer";
        arguments[4] = ignore_interrupt ? "--solution-arg=rendezvous-ignore-interrupt" : "--solution-arg=rendezvous";
        arguments[5] = "--solution-arg=" + solution_ready.string();
        arguments[6] = "--solution-arg=" + missing.string();
        arguments[7] = ignore_interrupt ? "--reference-arg=rendezvous-ignore-interrupt" : "--reference-arg=rendezvous";
        arguments[8] = "--reference-arg=" + reference_ready.string();
        arguments[9] = "--reference-arg=" + missing.string();
        std::vector<std::string> owned{SHRINK_CLI_PATH}; owned.insert(owned.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv; for (auto& argument : owned) argv.push_back(argument.data()); argv.push_back(nullptr);
        Child child{fork()};
        REQUIRE(child.pid >= 0);
        if (child.pid == 0) {
            if (setpgid(0, 0) < 0) _exit(91);
            const int output = open((workspace.directory / "stdout").c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            const int errors = open((workspace.directory / "stderr").c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            if (output < 0 || errors < 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(errors, STDERR_FILENO) < 0) _exit(92);
            close(output); close(errors);
            execv(SHRINK_CLI_PATH, argv.data());
            _exit(93);
        }
        struct ReadyFacts { pid_t process = 0, group = 0; int blocked = -1, default_handler = -1; } solution, reference;
        auto inspect = [](const fs::path& path, ReadyFacts& facts) {
            std::ifstream file(path);
            return static_cast<bool>(file >> facts.process >> facts.group >> facts.blocked >> facts.default_handler);
        };
        bool synchronized = false;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < end) {
            if (inspect(solution_ready, solution) && inspect(reference_ready, reference)) { synchronized = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool isolated_test_group = getpgid(child.pid) == child.pid;
        const int signalled = synchronized && isolated_test_group ? kill(-child.pid, SIGINT) : kill(child.pid, SIGINT);
        int status = 0;
        const bool exited = child.wait(status);
        REQUIRE(synchronized);
        REQUIRE(isolated_test_group);
        REQUIRE(signalled == 0);
        REQUIRE(exited);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 130);
        REQUIRE(solution.group != child.pid);
        REQUIRE(reference.group != child.pid);
        REQUIRE(solution.blocked == 0);
        REQUIRE(reference.blocked == 0);
        REQUIRE(solution.default_handler == (ignore_interrupt ? 0 : 1));
        REQUIRE(reference.default_handler == (ignore_interrupt ? 0 : 1));
        REQUIRE(workspace.read("result") == "previous\n");
        REQUIRE(workspace.read("stdout").empty());
        const auto errors = workspace.read("stderr");
        REQUIRE(errors.find("solution: signal 9") != std::string::npos);
        REQUIRE(errors.find("reference: signal 9") != std::string::npos);
        REQUIRE(errors.find("cleanup complete") != std::string::npos);
        REQUIRE(errors.find("No verified result") != std::string::npos);
        REQUIRE(kill(solution.process, 0) < 0);
        REQUIRE(errno == ESRCH);
        REQUIRE(kill(reference.process, 0) < 0);
        REQUIRE(errno == ESRCH);
    }
}

namespace {
// Script the system boundary that cannot be forced reliably on real children:
// termination is denied and cleanup remains pending until a later attempt.
class UnresolvedExecution final : public shrink::detail::ExecutionBackend {
public:
    CancellationSource source;
    ProgramRole cancel_on = ProgramRole::reference;
    std::array<unsigned, 2> launches{};
    std::array<unsigned, 2> stops{};
    std::array<bool, 2> active{};
    std::uint64_t clock = 0;
    bool delivered_partial = false;
    bool release = false;
    std::uint64_t now_ms() override { return clock; }
    std::optional<RunnerError> launch(ProgramRole role, const ProgramSpec&, std::string_view) override {
        ++launches[detail::program_index(role)]; active[detail::program_index(role)] = true;
        if (role == cancel_on) source.request_stop();
        return {};
    }
    std::optional<RunnerError> stop(ProgramRole role) override {
        ++stops[detail::program_index(role)];
        if (!release) return RunnerError{RunnerErrorCode::cleanup, role, EPERM, "scripted SIGKILL denial"};
        return {};
    }
    bool pending(ProgramRole role) const override { return active[detail::program_index(role)]; }
    PendingOwnership ownership(ProgramRole role) const override { return {role, 7000 + static_cast<int>(role), pending(role), pending(role) ? 3u : 0u}; }
    std::vector<detail::ExecutionEvent> service(CancellationToken, int wait_ms) override {
        clock += wait_ms;
        std::vector<detail::ExecutionEvent> events;
        for (auto role : detail::program_roles) {
            if (!pending(role)) continue;
            if (!delivered_partial) {
                events.push_back({role, detail::EventKind::setup_complete});
                events.push_back({role, detail::EventKind::stdout_data, role == ProgramRole::solution ? "1" : "2"});
            }
            if (release) {
                events.push_back({role, detail::EventKind::stdout_closed});
                events.push_back({role, detail::EventKind::stderr_closed});
                events.push_back({role, detail::EventKind::signalled, {}, SIGKILL});
                active[detail::program_index(role)] = false;
            }
        }
        delivered_partial = true;
        return events;
    }
};
}
TEST_CASE("CLI cancellation retains unresolved ownership and cleanup errors without accepting partial output", "[cli-cancellation]") {
    for (auto cancel_on : detail::program_roles) {
        Workspace workspace;
        auto backend = std::make_unique<UnresolvedExecution>(); auto* observed = backend.get();
        observed->cancel_on = cancel_on;
        Runner runner(std::move(backend));
        cli::InvocationServices services; services.runner = &runner; services.cancellation = observed->source.token();
        auto arguments = workspace.arguments();
        std::vector<std::string_view> views(arguments.begin(), arguments.end());
        std::ostringstream output, errors;
        const int status = cli::run(views, output, errors, services);
        const auto pending = runner.pending_ownership();
        const auto launches = observed->launches;
        const auto stops = observed->stops;
        const auto elapsed = observed->clock;
        // Always service test-owned resources before assertions can unwind.
        observed->release = true;
        auto cleanup = runner.cleanup();
        REQUIRE(status == 1);
        REQUIRE(cleanup.resolved);
        REQUIRE_FALSE(runner.has_pending_ownership());
        REQUIRE(pending.size() == (cancel_on == ProgramRole::solution ? 1 : 2));
        REQUIRE(launches == (cancel_on == ProgramRole::solution ? std::array<unsigned, 2>{1, 0} : std::array<unsigned, 2>{1, 1}));
        REQUIRE(observed->launches == launches);
        REQUIRE(stops[0] == 1);
        REQUIRE(stops[1] == (cancel_on == ProgramRole::reference ? 1 : 0));
        REQUIRE(elapsed == 1000);
        REQUIRE(workspace.read("result") == "previous\n");
        REQUIRE(output.str().empty());
        REQUIRE(errors.str().find("Observed: caller cancellation") != std::string::npos);
        REQUIRE(errors.str().find("scripted SIGKILL denial") != std::string::npos);
        REQUIRE(errors.str().find("Unresolved solution ownership") != std::string::npos);
        REQUIRE(errors.str().find("Process exit cannot guarantee continued cleanup") != std::string::npos);
        REQUIRE(errors.str().find("No verified result") != std::string::npos);
        REQUIRE(errors.str().find("both programs succeeded") == std::string::npos);
    }
}
