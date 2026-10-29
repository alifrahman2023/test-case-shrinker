#pragma once

#include "evaluator.hpp"
#include <functional>
#include <system_error>

namespace shrink {
enum class RunOutcome { invalid_request, baseline_rejected, completed, evaluation_limit,
                        time_limit, cancelled, operational_stop };
enum class RoundStatus { not_started, partial, complete };
enum class BaselineStatus { not_run, interrupted, agreement, solution_unsuccessful,
                            reference_unsuccessful, operational_failure, verified };
enum class RunErrorKind { validation, runner, cleanup, save, notification, search_unavailable };
struct RunError {
    RunErrorKind kind;
    std::string operation;
    std::optional<ValidationError> validation;
    std::optional<RunnerError> runner;
};
struct VerifiedInput {
    std::vector<std::int64_t> values;
    std::string normalized_input;
    std::uint64_t revision;
    std::shared_ptr<const Evaluation> evidence;
};
struct BaselineRecord {
    std::optional<std::size_t> original_length;
    BaselineStatus status = BaselineStatus::not_run;
    std::shared_ptr<const Evaluation> evaluation;
};
struct RunStatistics {
    std::uint64_t baseline_evaluations = 0;
    std::uint64_t candidate_evaluations = 0;
    std::uint64_t acceptances = 0;
    std::uint64_t lost_executions = 0;
    std::uint64_t inconclusive_executions = 0;
    std::uint64_t distinct_inconclusive_candidates = 0;
    std::uint64_t elapsed_ms = 0;
    RoundStatus round = RoundStatus::not_started;
};
enum class SaveKind { initial, periodic, final };
struct SaveRequest {
    std::string_view normalized_input;
    std::uint64_t revision;
    SaveKind kind;
};
enum class SaveErrorCode { create_temporary, write_temporary, close_temporary, replace, callback_exception };
struct SaveError {
    SaveErrorCode code;
    std::string operation;
    std::filesystem::path path;
    std::error_code system_error;
};
using SaveResult = std::optional<SaveError>;
enum class FinalSaveOutcome { not_attempted, succeeded, failed };
struct SaveFailure { SaveKind kind; std::uint64_t revision; SaveError error; };
struct SaveSummary {
    bool configured = false;
    std::optional<std::uint64_t> last_saved_revision;
    FinalSaveOutcome final_outcome = FinalSaveOutcome::not_attempted;
    std::vector<SaveFailure> failures;
};
enum class RunEventKind { baseline_verified, acceptance, progress };
struct RunEvent { RunEventKind kind; const VerifiedInput& best; const RunStatistics& statistics; };
// Callbacks execute synchronously on the calling thread. Keep them alive for
// the call, return promptly, and do not reenter the runner. Arguments borrow
// only through the callback; copy retained data. Blocked callbacks/filesystem
// operations can delay cancellation and deadlines.
struct RunHooks {
    std::function<SaveResult(const SaveRequest&)> save;
    std::function<void(const RunEvent&)> notify;
};
struct ObservedRunReasons {
    bool cancellation = false;
    bool time_limit = false;
    bool evaluation_limit = false;
};
struct ReductionResult {
    ObservedRunReasons observed_reasons;
    RunOutcome outcome = RunOutcome::operational_stop;
    std::optional<VerifiedInput> best;
    BaselineRecord baseline;
    std::shared_ptr<const Evaluation> operational_evaluation;
    RunStatistics statistics;
    std::vector<RunError> errors;
    SaveSummary saving;
    std::vector<PendingOwnership> pending_ownership;
};
ReductionResult reduce(ReductionRequest request, Runner& runner, CancellationToken cancellation = {},
                       const RunHooks& hooks = {});
}
