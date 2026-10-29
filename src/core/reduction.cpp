#include "reduction.hpp"
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <numeric>
#include <limits>

namespace shrink {
namespace {
std::uint64_t magnitude(std::int64_t value) {
    return value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
}
bool simpler(std::int64_t value, std::int64_t prior) {
    return magnitude(value) < magnitude(prior) || (magnitude(value) == magnitude(prior) && value > prior);
}
}
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
    auto collect_execution_errors = [&](const Evaluation& observed) {
        for (const auto& error : observed.execution.errors)
            result.errors.push_back({error.code == RunnerErrorCode::cleanup ? RunErrorKind::cleanup : RunErrorKind::runner,
                                     error.operation, {}, error});
        result.pending_ownership = runner.pending_ownership();
    };
    collect_execution_errors(*evaluation);
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
        std::uint64_t last_progress = 0;
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
            bool stopped = false;
            std::unordered_set<std::string> inconclusive;
            auto between_evaluations = [&] {
                if (observe_stops() || !result.errors.empty()) return true;
                if (pending_save && runner.now_ms() - last_save >= 1000) save(SaveKind::periodic);
                if (observe_stops() || !result.errors.empty()) return true;
                if (result.statistics.elapsed_ms - last_progress >= 1000) {
                    last_progress = result.statistics.elapsed_ms;
                    notify(RunEventKind::progress);
                }
                return observe_stops() || !result.errors.empty();
            };
            // A transform returns no value for an element being removed.
            // Generation, normalization and constraint checks share bounded
            // cancellation polling, including candidates that never execute.
            auto attempt = [&](auto transform) {
                if (stopped || between_evaluations()) { stopped = true; return false; }
                const auto& constraints = input.request.constraints;
                std::vector<std::int64_t> values;
                std::unordered_set<std::int64_t> unique;
                std::string body;
                for (std::size_t index = 0; index < result.best->values.size(); ++index) {
                    if (index % 1024 == 0 && observe_stops()) { stopped = true; return false; }
                    auto value = transform(index, result.best->values[index]);
                    if (!value) continue;
                    if (*value < constraints.min_value || *value > constraints.max_value) return false;
                    if (!values.empty() && ((constraints.ordering == Ordering::ascending && *value < values.back())
                        || (constraints.ordering == Ordering::descending && *value > values.back()))) return false;
                    if (constraints.unique && !unique.insert(*value).second) return false;
                    if (!values.empty()) body += ' ';
                    values.push_back(*value);
                    body += std::to_string(*value);
                }
                if (values.size() < static_cast<std::uint64_t>(constraints.min_length)
                    || values.size() > static_cast<std::uint64_t>(constraints.max_length)) return false;
                bool smaller = values.size() < result.best->values.size();
                if (values.size() == result.best->values.size()) {
                    for (std::size_t index = 0; index < values.size(); ++index) {
                        if (index % 1024 == 0 && observe_stops()) { stopped = true; return false; }
                        if (values[index] == result.best->values[index]) continue;
                        smaller = simpler(values[index], result.best->values[index]);
                        break;
                    }
                }
                if (!smaller) return false;
                if (observe_stops()) { stopped = true; return false; }
                if (result.observed_reasons.evaluation_limit) { stopped = true; return false; }
                auto text = std::to_string(values.size()) + '\n' + body + '\n';
                auto candidate = std::make_shared<Evaluation>(evaluate_candidate(runner, text,
                    input.request.solution, input.request.reference, input.request.limits, execution_cancellation));
                if (candidate->execution.programs.solution.launched || candidate->execution.programs.reference.launched)
                    ++result.statistics.candidate_evaluations;
                observe_stops();
                collect_execution_errors(*candidate);
                if (!candidate->execution.errors.empty()) result.operational_evaluation = candidate;
                if (!result.errors.empty() || observe_stops() || !candidate->outcome) { stopped = true; return false; }
                if (candidate->outcome == CandidateOutcome::preserved) {
                    result.best = VerifiedInput{std::move(values), std::move(text), result.best->revision + 1, candidate};
                    ++result.statistics.acceptances;
                    pending_save = true;
                    notify(RunEventKind::acceptance);
                    if (observe_stops() || !result.errors.empty()) stopped = true;
                    return true;
                }
                if (candidate->outcome == CandidateOutcome::lost) ++result.statistics.lost_executions;
                else {
                    ++result.statistics.inconclusive_executions;
                    inconclusive.insert(std::move(text));
                    result.statistics.distinct_inconclusive_candidates = inconclusive.size();
                }
                return false;
            };
            const auto simplest = std::clamp<std::int64_t>(0, input.request.constraints.min_value, input.request.constraints.max_value);
            auto simplify = [&](const std::vector<std::size_t>& positions) {
                if (positions.empty() || stopped) return;
                auto current = result.best->values[positions.front()];
                auto replace = [&](std::int64_t replacement) {
                    return attempt([&](std::size_t index, std::int64_t value) -> std::optional<std::int64_t> {
                        return std::binary_search(positions.begin(), positions.end(), index) ? replacement : value;
                    });
                };
                if (current == simplest) return;
                if (replace(simplest)) return;
                if (stopped) return;
                const auto unit = current < 0 ? current + 1 : current - 1;
                if (simpler(unit, current) && replace(unit)) {
                    current = unit;
                    auto rejected = simplest;
                    while (!stopped && magnitude(current) - magnitude(rejected) > 1) {
                        const auto middle = std::midpoint(rejected, current);
                        if (replace(middle)) current = middle;
                        else rejected = middle;
                    }
                }
                if (!stopped && current < 0 && current != std::numeric_limits<std::int64_t>::min()) replace(-current);
            };
            while (!stopped) {
                result.statistics.round = RoundStatus::partial;
                inconclusive.clear();
                result.statistics.distinct_inconclusive_candidates = 0;
                const auto starting_revision = result.best->revision;
                for (std::size_t chunk = std::max<std::size_t>(1, result.best->values.size() / 2); chunk && !stopped; chunk /= 2) {
                    for (std::size_t position = 0; position < result.best->values.size() && !stopped;) {
                        const auto count = std::min(chunk, result.best->values.size() - position);
                        if (!attempt([&](std::size_t index, std::int64_t value) -> std::optional<std::int64_t> {
                            if (index >= position && index - position < count) return {};
                            return value;
                        })) position += count;
                    }
                }
                // Visit distinct values in first-occurrence order. A subsequent
                // round revisits groups changed or merged by earlier acceptances.
                std::vector<std::int64_t> targets;
                std::unordered_set<std::int64_t> seen;
                for (auto value : result.best->values) {
                    if (observe_stops()) { stopped = true; break; }
                    if (seen.insert(value).second) targets.push_back(value);
                }
                for (auto target : targets) {
                    if (stopped) break;
                    std::vector<std::size_t> positions;
                    for (std::size_t index = 0; index < result.best->values.size(); ++index) {
                        if (index % 1024 == 0 && observe_stops()) { stopped = true; break; }
                        if (result.best->values[index] == target) positions.push_back(index);
                    }
                    simplify(positions);
                }
                std::unordered_map<std::int64_t, std::size_t> frequencies;
                for (auto value : result.best->values) {
                    if (observe_stops()) { stopped = true; break; }
                    ++frequencies[value];
                }
                for (std::size_t index = 0; index < result.best->values.size() && !stopped; ++index) {
                    if (observe_stops()) { stopped = true; break; }
                    const auto prior = result.best->values[index];
                    if (frequencies[prior] < 2) continue;
                    simplify({index});
                    --frequencies[prior];
                    ++frequencies[result.best->values[index]];
                }
                if (between_evaluations()) stopped = true;
                if (!stopped && result.best->revision == starting_revision) {
                    result.outcome = RunOutcome::completed;
                    result.statistics.round = RoundStatus::complete;
                    break;
                }
            }
            if (!result.errors.empty()) result.outcome = RunOutcome::operational_stop;
            else if (result.observed_reasons.cancellation) result.outcome = RunOutcome::cancelled;
            else if (result.observed_reasons.time_limit) result.outcome = RunOutcome::time_limit;
            else if (result.outcome != RunOutcome::completed && result.observed_reasons.evaluation_limit)
                result.outcome = RunOutcome::evaluation_limit;
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
