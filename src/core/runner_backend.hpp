#pragma once

// Internal clock/execution seam; not an extension interface or installed API.
#include "runner.hpp"

namespace shrink::detail {
enum class EventKind { setup_complete, stdout_data, stderr_data, stdout_closed,
                       stderr_closed, exited, signalled, error };
struct ExecutionEvent {
    int program;
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
    virtual std::optional<RunnerError> launch(int program, const ProgramSpec& spec,
                                             std::string_view input) = 0;
    virtual std::vector<ExecutionEvent> service(CancellationToken cancellation, int wait_ms) = 0;
    virtual std::optional<RunnerError> stop(int program) = 0;
    virtual bool pending(int program) const = 0;
    virtual std::optional<int> process_id(int) const { return {}; }
};
}
