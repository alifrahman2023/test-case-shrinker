#include "evaluator.hpp"

namespace shrink {
bool detail::execution_unsuccessful(const ExecutionEvidence& evidence) {
    return (evidence.exit_code && *evidence.exit_code != 0) || evidence.signal
        || !evidence.stop_reasons.empty();
}
namespace {
bool whitespace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
std::optional<TokenLocation> next_token(const std::string& bytes, std::size_t& cursor, CancellationToken cancellation) {
    while (cursor < bytes.size() && whitespace(bytes[cursor])) {
        if ((cursor & 16383) == 0 && cancellation.stop_requested()) return {};
        ++cursor;
    }
    const auto begin = cursor;
    while (cursor < bytes.size() && !whitespace(bytes[cursor])) {
        if ((cursor & 16383) == 0 && cancellation.stop_requested()) return {};
        ++cursor;
    }
    return TokenLocation{begin, cursor - begin, begin == bytes.size()};
}
bool equal_tokens(std::string_view solution, std::string_view reference, CancellationToken cancellation) {
    if (solution.size() != reference.size()) return false;
    for (std::size_t offset = 0; offset < solution.size(); offset += 16384) {
        if (cancellation.stop_requested()) return false;
        if (solution.substr(offset, 16384) != reference.substr(offset, 16384)) return false;
    }
    return true;
}
}
Evaluation evaluate_candidate(Runner& runner, std::string_view input, const ProgramSpec& solution,
                              const ProgramSpec& reference, const RunLimits& limits, CancellationToken cancellation) {
    Evaluation result{runner.run(input, solution, reference, limits, cancellation), {}, {}};
    if (cancellation.stop_requested()) result.execution.cancelled = true;
    if (!result.execution.errors.empty() || result.execution.cancelled) return result;
    auto classify = [&](CandidateOutcome outcome) {
        if (cancellation.stop_requested()) result.execution.cancelled = true;
        else result.outcome = outcome;
    };
    const auto& s = result.execution.programs[0];
    const auto& r = result.execution.programs[1];
    if (!s.successful()) { classify(CandidateOutcome::lost); return result; }
    if (!r.successful()) { classify(CandidateOutcome::inconclusive); return result; }
    std::size_t s_cursor = 0, r_cursor = 0;
    for (std::size_t position = 1;; ++position) {
        const auto st = next_token(s.stdout_bytes, s_cursor, cancellation);
        const auto rt = next_token(r.stdout_bytes, r_cursor, cancellation);
        if (!st || !rt || cancellation.stop_requested()) {
            result.execution.cancelled = true;
            return result;
        }
        if (st->end_of_output && rt->end_of_output) { classify(CandidateOutcome::lost); return result; }
        if (st->end_of_output != rt->end_of_output
            || !equal_tokens(std::string_view(s.stdout_bytes).substr(st->offset, st->length),
                             std::string_view(r.stdout_bytes).substr(rt->offset, rt->length), cancellation)) {
            classify(CandidateOutcome::preserved);
            if (result.outcome) result.disagreement = Disagreement{position, *st, *rt};
            return result;
        }
    }
}
}
