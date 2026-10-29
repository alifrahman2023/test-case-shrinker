#include "reduction.hpp"
#include <algorithm>
#include <unordered_set>

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
        if (result.best && input.request.limits.max_evaluations &&
            result.statistics.candidate_evaluations >= *input.request.limits.max_evaluations)
            result.observed_reasons.evaluation_limit = true;
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
        bool notifications_enabled = static_cast<bool>(hooks.notify);
        auto notify = [&](RunEventKind kind) {
            if (!notifications_enabled) return;
            try { hooks.notify({kind, *result.best, result.statistics}); }
            catch (...) {
                notifications_enabled = false;
                result.errors.push_back({RunErrorKind::notification, "notification callback threw", {}, {}});
            }
        };
        notify(RunEventKind::baseline_verified);
        std::uint64_t last_save = runner.now_ms();
        std::uint64_t last_progress = start;
        bool pending_save = false;
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
            } else {
                result.saving.last_saved_revision = result.best->revision;
                last_save = runner.now_ms();
                pending_save = false;
            }
        };
        save(SaveKind::initial);
        if (!result.errors.empty()) result.outcome = RunOutcome::operational_stop;
        else if (observe_stops()) result.outcome = result.observed_reasons.cancellation ? RunOutcome::cancelled : RunOutcome::time_limit;
        else if (input.request.limits.max_evaluations == 0) {
            result.outcome = RunOutcome::evaluation_limit;
            result.observed_reasons.evaluation_limit = true;
        }
        else {
            result.statistics.round = RoundStatus::partial;
            bool stopped = false;
            std::unordered_set<std::string> inconclusive;
            auto between_evaluations = [&] {
                if (observe_stops() || !result.errors.empty()) return true;
                if (pending_save && runner.now_ms() - last_save >= 1000) save(SaveKind::periodic);
                if (result.errors.empty() && runner.now_ms() - last_progress >= 1000) {
                    last_progress = runner.now_ms();
                    notify(RunEventKind::progress);
                }
                return observe_stops() || !result.errors.empty();
            };
            for (std::size_t chunk = std::max<std::size_t>(1, result.best->values.size() / 2); chunk; chunk /= 2) {
                for (std::size_t position = 0; position < result.best->values.size();) {
                    if (between_evaluations()) { stopped = true; break; }
                    const auto count = std::min(chunk, result.best->values.size() - position);
                    // A subsequence preserves value bounds, ordering and uniqueness.
                    // Length is the only declared constraint removal can invalidate.
                    if (result.best->values.size() - count < static_cast<std::uint64_t>(input.request.constraints.min_length)) {
                        position += count;
                        continue;
                    }
                    if (input.request.limits.max_evaluations && result.statistics.candidate_evaluations >= *input.request.limits.max_evaluations) {
                        result.observed_reasons.evaluation_limit = true;
                        result.outcome = RunOutcome::evaluation_limit;
                        stopped = true;
                        break;
                    }
                    std::vector<std::int64_t> values;
                    std::string text = std::to_string(result.best->values.size() - count) + '\n';
                    // Validate while generating. Removing elements cannot change
                    // their bounds or relative order, or introduce duplicates.
                    for (std::size_t index = 0; index < result.best->values.size(); ++index) {
                        if (index % 1024 == 0 && observe_stops()) { stopped = true; break; }
                        if (index >= position && index - position < count) continue;
                        if (!values.empty()) text += ' ';
                        values.push_back(result.best->values[index]);
                        text += std::to_string(values.back());
                    }
                    text += '\n';
                    if (stopped) break;
                    if (observe_stops()) { stopped = true; break; }
                    auto candidate = std::make_shared<Evaluation>(evaluate_candidate(runner, text,
                        input.request.solution, input.request.reference, input.request.limits, execution_cancellation));
                    if (candidate->execution.programs.solution.launched || candidate->execution.programs.reference.launched)
                        ++result.statistics.candidate_evaluations;
                    observe_stops();
                    for (const auto& error : candidate->execution.errors)
                        result.errors.push_back({error.code == RunnerErrorCode::cleanup ? RunErrorKind::cleanup : RunErrorKind::runner,
                                                 error.operation, {}, error});
                    result.pending_ownership = runner.pending_ownership();
                    if (!candidate->execution.errors.empty()) result.operational_evaluation = candidate;
                    if (!result.errors.empty() || observe_stops() || !candidate->outcome) { stopped = true; break; }
                    if (candidate->outcome == CandidateOutcome::preserved) {
                        result.best = VerifiedInput{std::move(values), std::move(text), result.best->revision + 1, candidate};
                        ++result.statistics.acceptances;
                        pending_save = true;
                        notify(RunEventKind::acceptance);
                    } else {
                        if (candidate->outcome == CandidateOutcome::lost) ++result.statistics.lost_executions;
                        else {
                            ++result.statistics.inconclusive_executions;
                            inconclusive.insert(std::move(text));
                            result.statistics.distinct_inconclusive_candidates = inconclusive.size();
                        }
                        position += count;
                    }
                }
                if (stopped) break;
            }
            between_evaluations();
            if (!result.errors.empty()) result.outcome = RunOutcome::operational_stop;
            else if (result.observed_reasons.cancellation) result.outcome = RunOutcome::cancelled;
            else if (result.observed_reasons.time_limit) result.outcome = RunOutcome::time_limit;
            else if (!stopped) result.errors.push_back({RunErrorKind::search_unavailable,
                "removal pass exhausted; remaining reduction passes are not yet available", {}, {}});
        }
        save(SaveKind::final);
        observe_stops();
        if (result.observed_reasons.cancellation &&
            (result.outcome == RunOutcome::evaluation_limit || result.outcome == RunOutcome::time_limit))
            result.outcome = RunOutcome::cancelled;
    }
    return result;
}
}
