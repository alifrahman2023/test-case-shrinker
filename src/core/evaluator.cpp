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
TokenLocation next_token(const std::string& bytes, std::size_t& cursor) {
    while (cursor < bytes.size() && whitespace(bytes[cursor])) ++cursor;
    const auto begin = cursor;
    while (cursor < bytes.size() && !whitespace(bytes[cursor])) ++cursor;
    return {begin, cursor - begin, begin == bytes.size()};
}
}
Evaluation evaluate_candidate(Runner& runner, std::string_view input, const ProgramSpec& solution,
                              const ProgramSpec& reference, const RunLimits& limits, CancellationToken cancellation) {
    Evaluation result{runner.run(input, solution, reference, limits, cancellation), {}, {}};
    if (!result.execution.errors.empty() || result.execution.cancelled) return result;
    const auto& s = result.execution.programs[0];
    const auto& r = result.execution.programs[1];
    if (!s.successful()) { result.outcome = CandidateOutcome::lost; return result; }
    if (!r.successful()) { result.outcome = CandidateOutcome::inconclusive; return result; }
    std::size_t s_cursor = 0, r_cursor = 0;
    for (std::size_t position = 1;; ++position) {
        const auto st = next_token(s.stdout_bytes, s_cursor);
        const auto rt = next_token(r.stdout_bytes, r_cursor);
        if (st.end_of_output && rt.end_of_output) { result.outcome = CandidateOutcome::lost; return result; }
        if (st.end_of_output != rt.end_of_output
            || std::string_view(s.stdout_bytes).substr(st.offset, st.length)
                != std::string_view(r.stdout_bytes).substr(rt.offset, rt.length)) {
            result.outcome = CandidateOutcome::preserved;
            result.disagreement = Disagreement{position, st, rt};
            return result;
        }
    }
}
}
