#pragma once

#include "coordinator.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <atomic>
#include <string>
#include <vector>

namespace shrink {
// Copyable caller-owned cancellation; runner waits are intercepted within 5 ms.
class CancellationToken {
public:
    bool stop_requested() const { return requested_ && requested_->load(); }
private:
    friend class CancellationSource;
    std::shared_ptr<std::atomic<bool>> requested_;
};
class CancellationSource {
public:
    CancellationSource() : requested_(std::make_shared<std::atomic<bool>>(false)) {}
    CancellationToken token() const { CancellationToken t; t.requested_ = requested_; return t; }
    void request_stop() const { requested_->store(true); }
private:
    std::shared_ptr<std::atomic<bool>> requested_;
};
inline constexpr std::size_t stdout_limit = 8'388'608;
inline constexpr std::size_t stderr_limit = 65'536;

enum class StopReason { timeout, stdout_overflow, cancellation, peer_failure, runner_error };
enum class RunnerErrorCode { configuration, launch, io, status, cleanup, pending_ownership };
struct RunnerError {
    RunnerErrorCode code;
    int program; // 0 solution, 1 reference, -1 whole runner
    int system_error = 0;
    std::string operation;
};
struct ExecutionEvidence {
    std::optional<int> process_id; // Diagnostic only; never signal or reap this identifier.
    bool launched = false;
    bool setup_complete = false; // Protocol completion is not proof that user code ran.
    bool output_complete = false;
    bool reaped = false;
    bool cleanup_complete = false;
    bool stderr_truncated = false;
    std::optional<int> exit_code;
    std::optional<int> signal;
    std::uint64_t elapsed_ms = 0;
    std::string stdout_bytes;
    std::string stderr_bytes;
    std::vector<StopReason> stop_reasons;
    bool successful() const;
    // Pending execution is neither successful nor unsuccessful.
    bool unsuccessful() const;
};
struct ExecutionPair {
    std::array<ExecutionEvidence, 2> programs;
    std::vector<RunnerError> errors;
    bool cancelled = false;
};

struct PendingOwnership {
    int program;
    std::optional<int> process_id; // Diagnostic only; ownership stays with Runner.
    bool awaiting_reap = false;
    std::size_t open_descriptors = 0;
    std::size_t uncertain_descriptors = 0;
};
struct CleanupResult {
    bool resolved = false;
    bool cancelled = false; // This attempt's cancellation, independent of prior execution.
    ExecutionPair execution; // Owning snapshot; includes the original operational errors.
    std::vector<RunnerError> errors; // Errors from this attempt only.
};
namespace detail { class ExecutionBackend; struct ExecutionEvent; }
// One synchronous evaluation or cleanup operation at a time. Concurrent calls
// and reentry are unsupported; only cancellation requests may be concurrent.
// Hosts must not reap these children or use incompatible SIGCHLD handling.
// A continuing host retains/services pending ownership. Destruction while it
// remains pending is a contract violation (terminates without waiting).
class Runner {
public:
    Runner();
    explicit Runner(std::unique_ptr<detail::ExecutionBackend> backend);
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    ExecutionPair run(std::string_view normalized_input, const ProgramSpec& solution,
                      const ProgramSpec& reference, const RunLimits& limits,
                      CancellationToken cancellation = {});
    bool has_pending_ownership() const;
    std::vector<PendingOwnership> pending_ownership() const;
    // Retry only existing ownership: never launches programs. One shared wait
    // for both programs, from zero through one second. Zero makes one nonblocking
    // pass; cancellation intercepts waits without abandoning resources.
    // Kernel operations/scheduling are not hard-bounded. Prior results stay owned
    // by their caller; this attempt returns a new snapshot and never erases errors.
    CleanupResult cleanup(std::chrono::milliseconds wait = std::chrono::milliseconds(1000),
                          CancellationToken cancellation = {});
private:
    std::unique_ptr<detail::ExecutionBackend> backend_;
    struct OutputCollection { bool stdout_closed = false; bool stderr_closed = false; };
    static bool collect_event(ExecutionEvidence& evidence, OutputCollection& collection,
                              const detail::ExecutionEvent& event);
    std::array<OutputCollection, 2> retained_collection_{};
    ExecutionPair retained_;
};
}
