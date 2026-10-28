#include "runner_backend.hpp"
#include "evaluator.hpp"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace shrink {
namespace {
using detail::EventKind;
using detail::ExecutionEvent;

class MacExecutionBackend final : public detail::ExecutionBackend {
    struct Child {
        pid_t pid = -1; // Only unreaped, exclusively owned direct children.
        std::array<int, 4> fd{-1, -1, -1, -1}; // stdin, stdout, stderr, setup
        std::string_view input;
        std::size_t sent = 0;
        std::string setup_bytes;
    };
    std::array<Child, 2> children_;
    mach_timebase_info_data_t timebase_{};

    static bool configure(int& fd) {
        if (fd < 3) {
            const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
            const int saved = errno;
            close(fd);
            fd = replacement;
            errno = saved;
            return fd >= 0;
        }
        return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
    }
    void close_fd(int program, int stream, std::vector<ExecutionEvent>& events) {
        int& fd = children_[program].fd[stream];
        if (fd < 0) return;
        const int owned = fd;
        fd = -1;
        if (close(owned) < 0) events.push_back({program, EventKind::error, {}, errno,
                                             RunnerErrorCode::cleanup, "close descriptor"});
    }
public:
    MacExecutionBackend() { mach_timebase_info(&timebase_); }
    std::uint64_t now_ms() override {
        // mach_continuous_time includes sleep; mach_absolute_time does not.
        return static_cast<std::uint64_t>(static_cast<long double>(mach_continuous_time())
                                         * timebase_.numer / timebase_.denom / 1'000'000);
    }
    std::optional<int> process_id(int program) const override { return children_[program].pid; }
    bool pending(int program) const override {
        const auto& child = children_[program];
        return child.pid > 0 || std::any_of(child.fd.begin(), child.fd.end(), [](int fd) { return fd >= 0; });
    }
    std::optional<RunnerError> launch(int program, const ProgramSpec& spec, std::string_view input) override {
        auto& child = children_[program];
        child = Child{};
        child.input = input;
        std::vector<std::string> arguments{spec.executable.string()};
        arguments.insert(arguments.end(), spec.arguments.begin(), spec.arguments.end());
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        std::array<std::array<int, 2>, 4> pipes;
        for (auto& pair : pipes) pair = {-1, -1};
        auto fail = [&](int error, const char* operation) -> std::optional<RunnerError> {
            for (auto& pair : pipes) for (int fd : pair) if (fd >= 0) close(fd);
            return RunnerError{RunnerErrorCode::launch, program, error, operation};
        };
        for (int stream = 0; stream < 4; ++stream) {
            const int result = stream == 0 ? socketpair(AF_UNIX, SOCK_STREAM, 0, pipes[stream].data())
                                           : pipe(pipes[stream].data());
            if (result < 0) return fail(errno, "create pipe");
            for (auto& fd : pipes[stream]) if (!configure(fd)) return fail(errno, "configure descriptor");
            const int parent = pipes[stream][stream == 0 ? 1 : 0];
            const int flags = fcntl(parent, F_GETFL);
            if (flags < 0 || fcntl(parent, F_SETFL, flags | O_NONBLOCK) < 0) return fail(errno, "nonblocking descriptor");
        }
        const int enabled = 1;
        if (setsockopt(pipes[0][1], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
            return fail(errno, "suppress input SIGPIPE");
        const auto pid = fork();
        if (pid < 0) return fail(errno, "fork");
        if (pid == 0) {
            // Only async-signal-safe operations between fork and exec. No signal
            // masks/dispositions were changed by the runner, so none leak.
            int error = 0;
            if (dup2(pipes[0][0], STDIN_FILENO) < 0 || dup2(pipes[1][1], STDOUT_FILENO) < 0
                || dup2(pipes[2][1], STDERR_FILENO) < 0) error = errno;
            const int setup = pipes[3][1];
            for (const auto& pair : pipes) for (int fd : pair) if (fd != setup) close(fd);
            if (!error) {
                execve(argv[0], argv.data(), environ);
                error = errno;
            }
            const char* bytes = reinterpret_cast<const char*>(&error);
            std::size_t sent = 0;
            while (sent < sizeof(error)) {
                const auto count = write(setup, bytes + sent, sizeof(error) - sent);
                if (count > 0) sent += static_cast<std::size_t>(count);
                else if (count < 0 && errno == EINTR) continue;
                else break;
            }
            _exit(127);
        }
        child.pid = pid;
        for (int stream = 0; stream < 4; ++stream) {
            const int parent_end = stream == 0 ? 1 : 0;
            child.fd[stream] = pipes[stream][parent_end];
            if (close(pipes[stream][1 - parent_end]) < 0)
                deferred_.push_back({program, EventKind::error, {}, errno, RunnerErrorCode::cleanup, "close unused descriptor"});
        }
        return {};
    }
    std::vector<ExecutionEvent> deferred_;
    std::vector<ExecutionEvent> service(CancellationToken, int wait_ms) override {
        auto events = std::move(deferred_);
        deferred_.clear();
        std::vector<pollfd> descriptors;
        std::vector<std::pair<int, int>> locations;
        for (int program = 0; program < 2; ++program) {
            auto& child = children_[program];
            if (child.fd[0] >= 0 && child.sent == child.input.size()) close_fd(program, 0, events);
            for (int stream = 0; stream < 4; ++stream) if (child.fd[stream] >= 0) {
                descriptors.push_back({child.fd[stream], static_cast<short>(stream == 0 ? POLLOUT : POLLIN), 0});
                locations.emplace_back(program, stream);
            }
        }
        const int ready = poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), wait_ms);
        if (ready < 0 && errno != EINTR) events.push_back({-1, EventKind::error, {}, errno, RunnerErrorCode::io, "poll"});
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            if (!descriptors[index].revents) continue;
            const auto [program, stream] = locations[index];
            auto& child = children_[program];
            if (descriptors[index].revents & POLLNVAL) {
                events.push_back({program, EventKind::error, {}, EBADF, RunnerErrorCode::io, "poll descriptor"});
                close_fd(program, stream, events);
                continue;
            }
            if (stream == 0) {
                const auto count = write(child.fd[0], child.input.data() + child.sent,
                                         std::min<std::size_t>(16384, child.input.size() - child.sent));
                if (count > 0) child.sent += static_cast<std::size_t>(count);
                else if (count < 0 && errno == EPIPE) close_fd(program, 0, events);
                else if (count < 0 && errno != EINTR && errno != EAGAIN) {
                    events.push_back({program, EventKind::error, {}, errno, RunnerErrorCode::io, "write input"});
                    close_fd(program, 0, events);
                }
                continue;
            }
            char buffer[16384];
            const auto count = read(child.fd[stream], buffer, sizeof(buffer));
            if (count > 0) {
                if (stream == 3) {
                    child.setup_bytes.append(buffer, static_cast<std::size_t>(count));
                    if (child.setup_bytes.size() >= sizeof(int)) {
                        int error;
                        __builtin_memcpy(&error, child.setup_bytes.data(), sizeof(error));
                        events.push_back({program, EventKind::error, {}, error, RunnerErrorCode::launch, "child setup/exec"});
                        close_fd(program, stream, events);
                    }
                } else events.push_back({program, stream == 1 ? EventKind::stdout_data : EventKind::stderr_data,
                                         std::string(buffer, static_cast<std::size_t>(count))});
            } else if (count == 0) {
                if (stream == 3 && !child.setup_bytes.empty())
                    events.push_back({program, EventKind::error, {}, EIO, RunnerErrorCode::launch, "incomplete setup report"});
                else events.push_back({program, stream == 3 ? EventKind::setup_complete
                                              : stream == 1 ? EventKind::stdout_closed : EventKind::stderr_closed, {}});
                close_fd(program, stream, events);
            } else if (errno != EINTR && errno != EAGAIN) {
                events.push_back({program, EventKind::error, {}, errno, RunnerErrorCode::io, "read output/setup"});
                close_fd(program, stream, events);
            }
        }
        for (int program = 0; program < 2; ++program) {
            auto& child = children_[program];
            if (child.pid <= 0) continue;
            int status = 0;
            const auto waited = waitpid(child.pid, &status, WNOHANG);
            if (waited == child.pid) {
                child.pid = -1; // End ownership before any further stop request.
                close_fd(program, 0, events);
                if (WIFEXITED(status)) events.push_back({program, EventKind::exited, {}, WEXITSTATUS(status)});
                else if (WIFSIGNALED(status)) events.push_back({program, EventKind::signalled, {}, WTERMSIG(status)});
                else events.push_back({program, EventKind::error, {}, EIO, RunnerErrorCode::status, "unexpected child status"});
            } else if (waited < 0 && errno != EINTR) {
                const int error = errno;
                // ECHILD means a host violated exclusive child ownership. Never
                // signal that identifier, which may already have been reused.
                if (error == ECHILD) child.pid = -1;
                events.push_back({program, EventKind::error, {}, error, RunnerErrorCode::status, "waitpid"});
            }
        }
        return events;
    }
    std::optional<RunnerError> stop(int program) override {
        auto& child = children_[program];
        std::vector<ExecutionEvent> events;
        close_fd(program, 0, events);
        deferred_.insert(deferred_.end(), events.begin(), events.end());
        if (child.pid > 0 && kill(child.pid, SIGKILL) < 0 && errno != ESRCH)
            return RunnerError{RunnerErrorCode::cleanup, program, errno, "SIGKILL"};
        return {};
    }
};
}

bool ExecutionEvidence::successful() const {
    return launched && setup_complete && output_complete && reaped && cleanup_complete
        && exit_code == 0 && !signal && stop_reasons.empty();
}
Runner::Runner() : backend_(std::make_unique<MacExecutionBackend>()) {}
Runner::Runner(std::unique_ptr<detail::ExecutionBackend> backend) : backend_(std::move(backend)) {}
Runner::~Runner() { if (has_pending_ownership()) std::terminate(); }
bool Runner::has_pending_ownership() const { return backend_->pending(0) || backend_->pending(1); }

ExecutionPair Runner::run(std::string_view input, const ProgramSpec& solution,
                          const ProgramSpec& reference, const RunLimits& limits, CancellationToken cancellation) {
    ExecutionPair result;
    if (has_pending_ownership()) {
        result.errors.push_back({RunnerErrorCode::pending_ownership, -1, 0, "runner retains prior execution"});
        return result;
    }
    if (limits.solution_timeout.count() <= 0 || limits.reference_timeout.count() <= 0) {
        result.errors.push_back({RunnerErrorCode::configuration, -1, 0, "program deadlines must be positive and finite"});
        return result;
    }
    for (const auto* spec : {&solution, &reference}) {
        if (spec->executable.empty() || spec->executable.native().find('\0') != std::string::npos
            || std::any_of(spec->arguments.begin(), spec->arguments.end(), [](const auto& arg) {
                return arg.find('\0') != std::string::npos;
            })) {
            result.errors.push_back({RunnerErrorCode::configuration, -1, 0, "invalid executable path or arguments"});
            return result;
        }
    }
    std::array<std::uint64_t, 2> start{};
    const std::array<const ProgramSpec*, 2> specs{&solution, &reference};
    const std::array<std::chrono::milliseconds, 2> timeouts{limits.solution_timeout, limits.reference_timeout};
    std::array<bool, 2> stdout_closed{}, stderr_closed{}, stopped{}, finished{};
    std::optional<std::uint64_t> cleanup_start;
    auto stop = [&](int program, StopReason reason) {
        auto& reasons = result.programs[program].stop_reasons;
        if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) reasons.push_back(reason);
        if (!stopped[program]) {
            stopped[program] = true;
            if (backend_->pending(program)) {
                if (!cleanup_start) cleanup_start = backend_->now_ms();
                if (auto error = backend_->stop(program)) result.errors.push_back(*error);
            }
        }
    };
    auto enforce_deadlines = [&] {
        const auto now = backend_->now_ms();
        for (int program = 0; program < 2; ++program) {
            auto& evidence = result.programs[program];
            if (!evidence.launched || finished[program] || stopped[program]) continue;
            if (now - start[program] >= static_cast<std::uint64_t>(timeouts[program].count())) {
                if (!evidence.setup_complete)
                    result.errors.push_back({RunnerErrorCode::launch, program, 0, "deadline expired during setup"});
                stop(program, StopReason::timeout);
            }
        }
    };
    auto enforce_stops = [&] {
        if (cancellation.stop_requested()) result.cancelled = true;
        if (!result.errors.empty() || result.cancelled) {
            for (int program = 0; program < 2; ++program) if (result.programs[program].launched)
                stop(program, result.cancelled ? StopReason::cancellation : StopReason::runner_error);
        } else if (detail::execution_unsuccessful(result.programs[0]) && backend_->pending(1)) {
            stop(1, StopReason::peer_failure);
        }
    };
    for (int program = 0; program < 2; ++program) {
        enforce_deadlines();
        enforce_stops();
        if (!result.errors.empty() || result.cancelled) break;
        start[program] = backend_->now_ms();
        if (auto error = backend_->launch(program, *specs[program], input)) {
            result.errors.push_back(*error);
            break;
        }
        result.programs[program].launched = true;
        result.programs[program].process_id = backend_->process_id(program);
    }
    enforce_deadlines();
    enforce_stops();
    while (has_pending_ownership()) {
        enforce_deadlines();
        enforce_stops();
        if (cleanup_start && !(stopped[0] && backend_->pending(0)) && !(stopped[1] && backend_->pending(1)))
            cleanup_start.reset();
        if (cleanup_start && backend_->now_ms() - *cleanup_start >= 1000) {
            result.errors.push_back({RunnerErrorCode::cleanup, -1, 0, "shared one-second cleanup deadline expired"});
            enforce_stops(); // Stop a still-running peer even when cleanup cannot wait further.
            break;
        }
        std::uint64_t wait_ms = 5;
        const auto now = backend_->now_ms();
        if (cleanup_start) {
            const auto elapsed = now - *cleanup_start;
            wait_ms = std::min(wait_ms, elapsed >= 1000 ? 0 : 1000 - elapsed);
        }
        for (int program = 0; program < 2; ++program) {
            if (result.programs[program].launched && !finished[program] && !stopped[program]) {
                const auto elapsed = now - start[program];
                const auto limit = static_cast<std::uint64_t>(timeouts[program].count());
                wait_ms = std::min(wait_ms, elapsed >= limit ? 0 : limit - elapsed);
            }
        }
        auto events = backend_->service(cancellation, static_cast<int>(wait_ms));
        // Collection is part of execution. Expiry wins over newly collected
        // completion, including after a machine sleep or a delayed scheduler.
        enforce_deadlines();
        for (const auto& event : events) {
            if (event.kind == EventKind::error) {
                result.errors.push_back({event.error_code, event.program, event.value, event.operation});
                continue;
            }
            auto& evidence = result.programs[event.program];
            switch (event.kind) {
                case EventKind::setup_complete: evidence.setup_complete = true; break;
                case EventKind::stdout_data: {
                    const auto remaining = stdout_limit - evidence.stdout_bytes.size();
                    evidence.stdout_bytes.append(event.bytes, 0, remaining);
                    if (event.bytes.size() > remaining) stop(event.program, StopReason::stdout_overflow);
                    break;
                }
                case EventKind::stderr_data: {
                    const auto remaining = stderr_limit - evidence.stderr_bytes.size();
                    evidence.stderr_bytes.append(event.bytes, 0, remaining);
                    evidence.stderr_truncated |= event.bytes.size() > remaining;
                    break;
                }
                case EventKind::stdout_closed: stdout_closed[event.program] = true; break;
                case EventKind::stderr_closed: stderr_closed[event.program] = true; break;
                case EventKind::exited: evidence.exit_code = event.value; evidence.reaped = true; break;
                case EventKind::signalled: evidence.signal = event.value; evidence.reaped = true; break;
                case EventKind::error: break;
            }
        }
        enforce_stops();
        for (int program = 0; program < 2; ++program) {
            auto& evidence = result.programs[program];
            if (evidence.launched && !finished[program]) {
                evidence.elapsed_ms = backend_->now_ms() - start[program];
                if (!backend_->pending(program)) finished[program] = true;
            }
        }
    }
    for (int program = 0; program < 2; ++program) {
        auto& evidence = result.programs[program];
        if (evidence.launched && !finished[program]) evidence.elapsed_ms = backend_->now_ms() - start[program];
        evidence.output_complete = stdout_closed[program] && stderr_closed[program];
        evidence.cleanup_complete = !backend_->pending(program);
    }
    return result;
}
}
