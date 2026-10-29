#include "reduction.hpp"

namespace shrink {
ReductionResult reduce(ReductionRequest request, Runner& runner, CancellationToken cancellation, const RunHooks& hooks) {
    ReductionResult result;
    result.saving.configured = static_cast<bool>(hooks.save);
    auto prepared = prepare_request(std::move(request));
    if (auto* error = std::get_if<ValidationError>(&prepared)) {
        result.outcome = RunOutcome::invalid_request;
        result.errors.push_back({RunErrorKind::validation, "validate request", *error, {}});
        return result;
    }
    auto input = std::get<PreparedRequest>(std::move(prepared));
    result.baseline.original_length = input.values.size();
    const auto start = runner.now_ms();
    auto observe_stops = [&] {
        result.statistics.elapsed_ms = runner.now_ms() - start;
        if (cancellation.stop_requested()) result.observed_reasons.cancellation = true;
        if (input.request.limits.time_limit && result.statistics.elapsed_ms >=
            static_cast<std::uint64_t>(input.request.limits.time_limit->count()))
            result.observed_reasons.time_limit = true;
        return result.observed_reasons.cancellation || result.observed_reasons.time_limit;
    };
    auto execution_cancellation = cancellation.with_stop_check(observe_stops);
    auto evaluation = std::make_shared<Evaluation>(evaluate_candidate(runner, input.normalized_input,
        input.request.solution, input.request.reference, input.request.limits, execution_cancellation));
    observe_stops();
    result.statistics.baseline_evaluations = evaluation->execution.programs.solution.launched || evaluation->execution.programs.reference.launched ? 1 : 0;
    result.baseline.evaluation = evaluation;
    for (const auto& error : evaluation->execution.errors)
        result.errors.push_back({error.code == RunnerErrorCode::cleanup ? RunErrorKind::cleanup : RunErrorKind::runner,
                                 error.operation, {}, error});
    result.pending_ownership = runner.pending_ownership();
    if (!result.errors.empty()) {
        result.baseline.status = BaselineStatus::operational_failure;
    } else if (evaluation->execution.cancelled || result.observed_reasons.cancellation || result.observed_reasons.time_limit) {
        result.outcome = result.observed_reasons.cancellation ? RunOutcome::cancelled : RunOutcome::time_limit;
        result.baseline.status = BaselineStatus::interrupted;
    } else if (evaluation->outcome != CandidateOutcome::preserved) {
        result.outcome = RunOutcome::baseline_rejected;
        const auto& programs = evaluation->execution.programs;
        result.baseline.status = !programs.solution.successful() ? BaselineStatus::solution_unsuccessful
            : !programs.reference.successful() ? BaselineStatus::reference_unsuccessful : BaselineStatus::agreement;
        if (evaluation->outcome == CandidateOutcome::lost) ++result.statistics.lost_executions;
        if (evaluation->outcome == CandidateOutcome::inconclusive) ++result.statistics.inconclusive_executions;
    } else {
        result.baseline.status = BaselineStatus::verified;
        result.best = VerifiedInput{std::move(input.values), std::move(input.normalized_input), 1, evaluation};
        if (hooks.notify) {
            try { hooks.notify({RunEventKind::baseline_verified, *result.best, result.statistics}); }
            catch (...) {
                // No further notification is issued after a throwing callback.
                result.errors.push_back({RunErrorKind::notification, "baseline notification threw", {}, {}});
            }
        }
        auto save = [&](SaveKind kind) {
            if (!hooks.save) return;
            SaveResult error;
            try { error = hooks.save({result.best->normalized_input, result.best->revision, kind}); }
            catch (...) { error = SaveError{SaveErrorCode::callback_exception, "save callback threw", {}, {}}; }
            if (kind == SaveKind::final)
                result.saving.final_outcome = error ? FinalSaveOutcome::failed : FinalSaveOutcome::succeeded;
            if (error) {
                result.saving.failures.push_back({kind, result.best->revision, *error});
                result.errors.push_back({RunErrorKind::save, error->operation, {}, {}});
            } else result.saving.last_saved_revision = result.best->revision;
        };
        save(SaveKind::initial);
        if (!result.errors.empty()) result.outcome = RunOutcome::operational_stop;
        else if (observe_stops()) result.outcome = result.observed_reasons.cancellation ? RunOutcome::cancelled : RunOutcome::time_limit;
        else if (input.request.limits.max_evaluations == 0) {
            result.outcome = RunOutcome::evaluation_limit;
            result.observed_reasons.evaluation_limit = true;
        }
        else result.errors.push_back({RunErrorKind::search_unavailable, "candidate search is not yet available; use --max-evaluations 0", {}, {}});
        save(SaveKind::final);
        observe_stops();
    }
    return result;
}
}
