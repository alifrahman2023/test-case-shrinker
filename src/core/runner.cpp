#include "runner_backend.hpp"

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
using detail::program_index;
using detail::program_roles;
using detail::program_count;

enum class Stream { input, output, error, setup };
constexpr std::array streams{Stream::input, Stream::output, Stream::error, Stream::setup};
constexpr std::size_t stream_index(Stream stream) { return static_cast<std::size_t>(stream); }
constexpr std::size_t read_end = 0;
constexpr std::size_t write_end = 1;
constexpr std::size_t io_chunk_bytes = 16 * 1024;
// Keep owned descriptors outside the standard streams during dup2/close setup.
constexpr int first_nonstandard_descriptor = STDERR_FILENO + 1;
constexpr std::size_t parent_end(Stream stream) { return stream == Stream::input ? write_end : read_end; }
constexpr std::size_t child_end(Stream stream) { return stream == Stream::input ? read_end : write_end; }

class MacExecutionBackend final : public detail::ExecutionBackend {
    struct Child {
        pid_t pid = -1; // Only unreaped, exclusively owned direct children.
        std::array<int, streams.size()> descriptors{-1, -1, -1, -1};
        int& descriptor(Stream stream) { return descriptors[stream_index(stream)]; }
        std::string_view input;
        std::size_t sent = 0;
        std::string setup_bytes;
    };
    std::array<Child, program_count> children_;
    mach_timebase_info_data_t timebase_{};
    // A failed close can leave release uncertain. Keep responsibility and block
    // reuse of the runner; never retry a number that another thread may reuse.
    std::array<std::vector<int>, program_count> uncertain_closes_;
    int close_owned(ProgramRole program, int fd) {
        if (close(fd) == 0) return 0;
        const int error = errno;
        uncertain_closes_[program_index(program)].push_back(fd);
        return error;
    }

    bool configure(ProgramRole program, int& fd) {
        if (fd < first_nonstandard_descriptor) {
            const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, first_nonstandard_descriptor);
            const int saved = errno;
            const int close_error = close_owned(program, fd);
            fd = replacement;
            errno = close_error ? close_error : saved;
            return fd >= 0 && !close_error;
        }
        return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
    }
    void close_fd(ProgramRole program, Stream stream, std::vector<ExecutionEvent>& events) {
        int& fd = children_[program_index(program)].descriptor(stream);
        if (fd < 0) return;
        const int owned = fd;
        fd = -1;
        if (const int error = close_owned(program, owned))
            events.push_back({program, EventKind::error, {}, error, RunnerErrorCode::cleanup, "close descriptor"});
    }
public:
    MacExecutionBackend() { mach_timebase_info(&timebase_); }
    std::uint64_t now_ms() override {
        // mach_continuous_time includes sleep; mach_absolute_time does not.
        return static_cast<std::uint64_t>(static_cast<long double>(mach_continuous_time())
                                         * timebase_.numer / timebase_.denom / 1'000'000);
    }
    std::optional<int> process_id(ProgramRole program) const override { return children_[program_index(program)].pid; }
    PendingOwnership ownership(ProgramRole program) const override {
        const auto& child = children_[program_index(program)];
        return {program, child.pid > 0 ? std::optional<int>(child.pid) : std::nullopt, child.pid > 0,
                static_cast<std::size_t>(std::count_if(child.descriptors.begin(), child.descriptors.end(), [](int fd) { return fd >= 0; })),
                uncertain_closes_[program_index(program)].size()};
    }
    bool pending(ProgramRole program) const override {
        const auto& child = children_[program_index(program)];
        return child.pid > 0 || !uncertain_closes_[program_index(program)].empty()
            || std::any_of(child.descriptors.begin(), child.descriptors.end(), [](int fd) { return fd >= 0; });
    }
    std::optional<RunnerError> launch(ProgramRole program, const ProgramSpec& spec, std::string_view input) override {
        auto& child = children_[program_index(program)];
        child = Child{};
        child.input = input;
        std::vector<std::string> arguments{spec.executable.string()};
        arguments.insert(arguments.end(), spec.arguments.begin(), spec.arguments.end());
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        // Each pipe has a read end and a write end, independently of program count.
        std::array<std::array<int, 2>, streams.size()> pipes;
        for (auto& pair : pipes) pair = {-1, -1};
        auto pipe_for = [&](Stream stream) -> std::array<int, 2>& { return pipes[stream_index(stream)]; };
        auto fail = [&](int error, const char* operation) -> std::optional<RunnerError> {
            int cleanup_error = 0;
            for (auto& pair : pipes) for (int fd : pair) if (fd >= 0) {
                const int closed = close_owned(program, fd);
                if (closed) cleanup_error = closed;
            }
            if (cleanup_error || !uncertain_closes_[program_index(program)].empty())
                return RunnerError{RunnerErrorCode::cleanup, program, cleanup_error ? cleanup_error : error,
                                   std::string("descriptor release uncertain after ") + operation};
            return RunnerError{RunnerErrorCode::launch, program, error, operation};
        };
        for (const auto stream : streams) {
            const int result = stream == Stream::input ? socketpair(AF_UNIX, SOCK_STREAM, 0, pipe_for(stream).data())
                                           : pipe(pipe_for(stream).data());
            if (result < 0) return fail(errno, "create pipe");
            for (auto& fd : pipe_for(stream)) if (!configure(program, fd)) return fail(errno, "configure descriptor");
            const int parent = pipe_for(stream)[parent_end(stream)];
            const int flags = fcntl(parent, F_GETFL);
            if (flags < 0 || fcntl(parent, F_SETFL, flags | O_NONBLOCK) < 0) return fail(errno, "nonblocking descriptor");
        }
        const int enabled = 1;
        if (setsockopt(pipe_for(Stream::input)[write_end], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
            return fail(errno, "suppress input SIGPIPE");
        const auto pid = fork();
        if (pid < 0) return fail(errno, "fork");
        if (pid == 0) {
            // Only async-signal-safe operations between fork and exec. No signal
            // masks/dispositions were changed by the runner, so none leak.
            int error = 0;
            if (dup2(pipe_for(Stream::input)[read_end], STDIN_FILENO) < 0 || dup2(pipe_for(Stream::output)[write_end], STDOUT_FILENO) < 0
                || dup2(pipe_for(Stream::error)[write_end], STDERR_FILENO) < 0) error = errno;
            const int setup = pipe_for(Stream::setup)[write_end];
            for (const auto& pair : pipes) for (int fd : pair) if (fd != setup) {
                if (close(fd) < 0 && !error) error = errno;
            }
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
        for (const auto stream : streams) {
            child.descriptor(stream) = pipe_for(stream)[parent_end(stream)];
            if (const int error = close_owned(program, pipe_for(stream)[child_end(stream)]))
                deferred_.push_back({program, EventKind::error, {}, error, RunnerErrorCode::cleanup, "close unused descriptor"});
        }
        return {};
    }
    std::vector<ExecutionEvent> deferred_;
    std::vector<ExecutionEvent> service(CancellationToken, int wait_ms) override {
        auto events = std::move(deferred_);
        deferred_.clear();
        for (const auto program : program_roles) {
            // A missing descriptor proves release. An occupied number may have
            // been reused: inspect it read-only and never blindly close it again.
            std::erase_if(uncertain_closes_[program_index(program)], [&](int fd) {
                if (fcntl(fd, F_GETFD) >= 0) return false;
                const int error = errno;
                if (error == EBADF) return true;
                if (error != EINTR) events.push_back({program, EventKind::error, {}, error,
                                                     RunnerErrorCode::cleanup, "inspect uncertain descriptor"});
                return false;
            });
        }
        std::vector<pollfd> descriptors;
        std::vector<std::pair<ProgramRole, Stream>> locations;
        for (const auto program : program_roles) {
            auto& child = children_[program_index(program)];
            if (child.descriptor(Stream::input) >= 0 && child.sent == child.input.size()) close_fd(program, Stream::input, events);
            for (const auto stream : streams) if (child.descriptor(stream) >= 0) {
                descriptors.push_back({child.descriptor(stream), static_cast<short>(stream == Stream::input ? POLLOUT : POLLIN), 0});
                locations.emplace_back(program, stream);
            }
        }
        const int ready = poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), wait_ms);
        if (ready < 0 && errno != EINTR) events.push_back({std::nullopt, EventKind::error, {}, errno, RunnerErrorCode::io, "poll"});
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            if (!descriptors[index].revents) continue;
            const auto [program, stream] = locations[index];
            auto& child = children_[program_index(program)];
            if (descriptors[index].revents & POLLNVAL) {
                events.push_back({program, EventKind::error, {}, EBADF, RunnerErrorCode::io, "poll descriptor"});
                close_fd(program, stream, events);
                continue;
            }
            if (stream == Stream::input) {
                const auto count = write(child.descriptor(Stream::input), child.input.data() + child.sent,
                                         std::min<std::size_t>(io_chunk_bytes, child.input.size() - child.sent));
                if (count > 0) child.sent += static_cast<std::size_t>(count);
                else if (count < 0 && errno == EPIPE) close_fd(program, Stream::input, events);
                else if (count < 0 && errno != EINTR && errno != EAGAIN) {
                    events.push_back({program, EventKind::error, {}, errno, RunnerErrorCode::io, "write input"});
                    close_fd(program, Stream::input, events);
                }
                continue;
            }
            char buffer[io_chunk_bytes];
            const auto count = read(child.descriptor(stream), buffer, sizeof(buffer));
            if (count > 0) {
                if (stream == Stream::setup) {
                    child.setup_bytes.append(buffer, static_cast<std::size_t>(count));
                    if (child.setup_bytes.size() >= sizeof(int)) {
                        int error;
                        __builtin_memcpy(&error, child.setup_bytes.data(), sizeof(error));
                        events.push_back({program, EventKind::error, {}, error, RunnerErrorCode::launch, "child setup/exec"});
                        close_fd(program, stream, events);
                    }
                } else events.push_back({program, stream == Stream::output ? EventKind::stdout_data : EventKind::stderr_data,
                                         std::string(buffer, static_cast<std::size_t>(count))});
            } else if (count == 0) {
                if (stream == Stream::setup && !child.setup_bytes.empty())
                    events.push_back({program, EventKind::error, {}, EIO, RunnerErrorCode::launch, "incomplete setup report"});
                else events.push_back({program, stream == Stream::setup ? EventKind::setup_complete
                                              : stream == Stream::output ? EventKind::stdout_closed : EventKind::stderr_closed, {}});
                close_fd(program, stream, events);
            } else if (errno != EINTR && errno != EAGAIN) {
                events.push_back({program, EventKind::error, {}, errno, RunnerErrorCode::io, "read output/setup"});
                close_fd(program, stream, events);
            }
        }
        for (const auto program : program_roles) {
            auto& child = children_[program_index(program)];
            if (child.pid <= 0) continue;
            int status = 0;
            const auto waited = waitpid(child.pid, &status, WNOHANG);
            if (waited == child.pid) {
                // Only terminal status releases ownership. A traced/stopped
                // child may produce a nonterminal status even without WUNTRACED.
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    child.pid = -1; // End ownership before any further stop request.
                    close_fd(program, Stream::input, events);
                }
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
    std::optional<RunnerError> stop(ProgramRole program) override {
        auto& child = children_[program_index(program)];
        std::vector<ExecutionEvent> events;
        close_fd(program, Stream::input, events);
        deferred_.insert(deferred_.end(), events.begin(), events.end());
        if (child.pid > 0 && kill(child.pid, SIGKILL) < 0 && errno != ESRCH)
            return RunnerError{RunnerErrorCode::cleanup, program, errno, "SIGKILL"};
        return {};
    }
};
}

bool ExecutionEvidence::unsuccessful() const {
    return (exit_code && *exit_code != 0) || signal || !stop_reasons.empty();
}
bool ExecutionEvidence::successful() const {
    return launched && setup_complete && output_complete && reaped && cleanup_complete
        && exit_code == 0 && !signal && stop_reasons.empty();
}
std::unique_ptr<detail::ExecutionBackend> detail::make_mac_execution_backend() {
    return std::make_unique<MacExecutionBackend>();
}
Runner::Runner() : backend_(detail::make_mac_execution_backend()) {}
Runner::Runner(std::unique_ptr<detail::ExecutionBackend> backend) : backend_(std::move(backend)) {}
Runner::~Runner() { if (has_pending_ownership()) std::terminate(); }
bool Runner::has_pending_ownership() const { return backend_->pending(ProgramRole::solution) || backend_->pending(ProgramRole::reference); }

std::vector<PendingOwnership> Runner::pending_ownership() const {
    std::vector<PendingOwnership> result;
    for (const auto program : program_roles) if (backend_->pending(program)) {
        auto ownership = backend_->ownership(program);
        if (!ownership.process_id) ownership.process_id = retained_.programs.for_role(program).process_id;
        result.push_back(ownership);
    }
    return result;
}
bool Runner::collect_event(ExecutionEvidence& evidence, OutputCollection& collection,
                           const detail::ExecutionEvent& event) {
    bool overflow = false;
    switch (event.kind) {
        case EventKind::setup_complete: evidence.setup_complete = true; break;
        case EventKind::stdout_data: {
            const auto remaining = stdout_limit - evidence.stdout_bytes.size();
            evidence.stdout_bytes.append(event.bytes, 0, remaining);
            overflow = event.bytes.size() > remaining;
            if (overflow && std::find(evidence.stop_reasons.begin(), evidence.stop_reasons.end(), StopReason::stdout_overflow) == evidence.stop_reasons.end())
                evidence.stop_reasons.push_back(StopReason::stdout_overflow);
            break;
        }
        case EventKind::stderr_data: {
            const auto remaining = stderr_limit - evidence.stderr_bytes.size();
            evidence.stderr_bytes.append(event.bytes, 0, remaining);
            evidence.stderr_truncated |= event.bytes.size() > remaining;
            break;
        }
        case EventKind::stdout_closed: collection.stdout_closed = true; break;
        case EventKind::stderr_closed: collection.stderr_closed = true; break;
        case EventKind::exited: evidence.exit_code = event.value; evidence.reaped = true; break;
        case EventKind::signalled: evidence.signal = event.value; evidence.reaped = true; break;
        case EventKind::error: break;
    }
    evidence.output_complete = collection.stdout_closed && collection.stderr_closed;
    return overflow;
}

CleanupResult Runner::cleanup(std::chrono::milliseconds wait, CancellationToken cancellation) {
    CleanupResult result;
    if (wait.count() < 0 || wait.count() > 1000) {
        result.errors.push_back({RunnerErrorCode::configuration, std::nullopt, 0, "cleanup wait must be between zero and one second"});
        result.resolved = !has_pending_ownership();
        result.cancelled = cancellation.stop_requested();
        result.execution = retained_;
        return result;
    }
    const auto start = backend_->now_ms();
    for (const auto program : program_roles) if (backend_->pending(program)) {
        if (auto error = backend_->stop(program)) result.errors.push_back(*error);
    }
    while (has_pending_ownership()) {
        const auto elapsed = backend_->now_ms() - start;
        result.cancelled = cancellation.stop_requested();
        const auto limit = static_cast<std::uint64_t>(wait.count());
        const int wait_ms = result.cancelled || elapsed >= limit ? 0
            : static_cast<int>(std::min<std::uint64_t>(5, limit - elapsed));
        for (const auto& event : backend_->service(cancellation, wait_ms)) {
            if (event.kind == EventKind::error) {
                result.errors.push_back({event.error_code, event.program, event.value, event.operation});
                continue;
            }
            collect_event(retained_.programs.for_role(*event.program), retained_collection_[program_index(*event.program)], event);
        }
        if (!has_pending_ownership()) break;
        result.cancelled = cancellation.stop_requested();
        if (result.cancelled || backend_->now_ms() - start >= static_cast<std::uint64_t>(wait.count())) {
            if (!result.cancelled) result.errors.push_back({RunnerErrorCode::cleanup, std::nullopt, 0, "bounded cleanup attempt expired"});
            break;
        }
    }
    for (const auto program : program_roles) retained_.programs.for_role(program).cleanup_complete = !backend_->pending(program);
    result.resolved = !has_pending_ownership();
    result.cancelled = cancellation.stop_requested();
    result.execution = retained_;
    return result;
}

ExecutionPair Runner::run(std::string_view input, const ProgramSpec& solution,
                          const ProgramSpec& reference, const RunLimits& limits, CancellationToken cancellation) {
    ExecutionPair result;
    if (has_pending_ownership()) {
        result.errors.push_back({RunnerErrorCode::pending_ownership, std::nullopt, 0, "runner retains prior execution"});
        return result;
    }
    if (const auto error = validate_execution_configuration(solution, reference, limits)) {
        result.errors.push_back({RunnerErrorCode::configuration, std::nullopt, 0, error->message});
        return result;
    }
    retained_ = {};
    retained_collection_ = {};
    struct ProgramState {
        std::uint64_t start = 0;
        std::chrono::milliseconds timeout;
        OutputCollection collection{};
        bool stopped = false;
        bool finished = false;
    };
    std::array<ProgramState, program_count> states{{{0, limits.solution_timeout}, {0, limits.reference_timeout}}};
    const std::array<const ProgramSpec*, program_count> specs{&solution, &reference};
    std::optional<std::uint64_t> cleanup_start;
    auto stop = [&](ProgramRole program, StopReason reason) {
        auto& reasons = result.programs.for_role(program).stop_reasons;
        if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) reasons.push_back(reason);
        auto& state = states[program_index(program)];
        if (!state.stopped) {
            state.stopped = true;
            if (backend_->pending(program)) {
                if (!cleanup_start) cleanup_start = backend_->now_ms();
                if (auto error = backend_->stop(program)) result.errors.push_back(*error);
            }
        }
    };
    auto enforce_deadlines = [&] {
        const auto now = backend_->now_ms();
        for (const auto program : program_roles) {
            auto& evidence = result.programs.for_role(program);
            const auto& state = states[program_index(program)];
            if (!evidence.launched || state.finished || state.stopped) continue;
            if (now - state.start >= static_cast<std::uint64_t>(state.timeout.count())) {
                if (!evidence.setup_complete)
                    result.errors.push_back({RunnerErrorCode::launch, program, 0, "deadline expired during setup"});
                stop(program, StopReason::timeout);
            }
        }
    };
    auto enforce_stops = [&] {
        if (cancellation.stop_requested()) result.cancelled = true;
        if (!result.errors.empty() || result.cancelled) {
            for (const auto program : program_roles)
                if (result.programs.for_role(program).launched || backend_->pending(program)) stop(program, result.cancelled ? StopReason::cancellation : StopReason::runner_error);
        } else if (result.programs.solution.unsuccessful()
                   && backend_->pending(ProgramRole::reference)) {
            stop(ProgramRole::reference, StopReason::peer_failure);
        }
    };
    for (const auto program : program_roles) {
        enforce_deadlines();
        enforce_stops();
        if (!result.errors.empty() || result.cancelled) break;
        states[program_index(program)].start = backend_->now_ms();
        if (auto error = backend_->launch(program, *specs[program_index(program)], input)) {
            result.errors.push_back(*error);
            break;
        }
        result.programs.for_role(program).launched = true;
        result.programs.for_role(program).process_id = backend_->process_id(program);
    }
    enforce_deadlines();
    enforce_stops();
    while (has_pending_ownership()) {
        enforce_deadlines();
        enforce_stops();
        if (cleanup_start
            && !(states[program_index(ProgramRole::solution)].stopped && backend_->pending(ProgramRole::solution))
            && !(states[program_index(ProgramRole::reference)].stopped && backend_->pending(ProgramRole::reference)))
            cleanup_start.reset();
        if (cleanup_start && backend_->now_ms() - *cleanup_start >= 1000) {
            result.errors.push_back({RunnerErrorCode::cleanup, std::nullopt, 0, "shared one-second cleanup deadline expired"});
            enforce_stops(); // Stop a still-running peer even when cleanup cannot wait further.
            break;
        }
        std::uint64_t wait_ms = 5;
        const auto now = backend_->now_ms();
        if (cleanup_start) {
            const auto elapsed = now - *cleanup_start;
            wait_ms = std::min(wait_ms, elapsed >= 1000 ? 0 : 1000 - elapsed);
        }
        for (const auto program : program_roles) {
            const auto& state = states[program_index(program)];
            if (result.programs.for_role(program).launched && !state.finished && !state.stopped) {
                const auto elapsed = now - state.start;
                const auto limit = static_cast<std::uint64_t>(state.timeout.count());
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
            if (collect_event(result.programs.for_role(*event.program), states[program_index(*event.program)].collection, event))
                stop(*event.program, StopReason::stdout_overflow);
        }
        enforce_stops();
        for (const auto program : program_roles) {
            auto& evidence = result.programs.for_role(program);
            auto& state = states[program_index(program)];
            if (evidence.launched && !state.finished) {
                evidence.elapsed_ms = backend_->now_ms() - state.start;
                if (!backend_->pending(program)) state.finished = true;
            }
        }
    }
    for (const auto program : program_roles) {
        auto& evidence = result.programs.for_role(program);
        const auto& state = states[program_index(program)];
        if (evidence.launched && !state.finished) evidence.elapsed_ms = backend_->now_ms() - state.start;
        evidence.output_complete = state.collection.stdout_closed && state.collection.stderr_closed;
        evidence.cleanup_complete = !backend_->pending(program);
        retained_collection_[program_index(program)] = state.collection;
    }
    if (has_pending_ownership()) retained_ = result;
    return result;
}
}
