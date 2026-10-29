#pragma once

// Internal clock/execution seam; not an extension interface or installed API.
#include "runner.hpp"

namespace shrink::detail {
constexpr std::size_t program_index(ProgramRole program) { return static_cast<std::size_t>(program); }

enum class EventKind { setup_complete, stdout_data, stderr_data, stdout_closed,
                       stderr_closed, exited, signalled, error };
struct ExecutionEvent {
    std::optional<ProgramRole> program;
    EventKind kind;
    std::string bytes{};
    int value = 0;
    RunnerErrorCode error_code = RunnerErrorCode::io;
    std::string operation{};
};
class ExecutionBackend {
public:
    virtual ~ExecutionBackend() = default;
    // Sleep-inclusive monotonic elapsed milliseconds, independent of wall time.
    virtual std::uint64_t now_ms() = 0;
    virtual std::optional<RunnerError> launch(ProgramRole program, const ProgramSpec& spec,
                                             std::string_view input) = 0;
    virtual std::vector<ExecutionEvent> service(CancellationToken cancellation, int wait_ms) = 0;
    virtual std::optional<RunnerError> stop(ProgramRole program) = 0;
    virtual bool pending(ProgramRole program) const = 0;
    virtual std::optional<int> process_id(ProgramRole) const { return {}; }
    virtual PendingOwnership ownership(ProgramRole program) const = 0;
};
std::unique_ptr<ExecutionBackend> make_mac_execution_backend();
}
