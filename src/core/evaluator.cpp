#include "evaluator.hpp"

namespace shrink {
namespace {
// Bound scanning/comparison work between cancellation checks to 16 KiB.
constexpr std::size_t cancellation_check_interval_bytes = 16 * 1024;
bool whitespace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
std::optional<TokenLocation> next_token(const std::string& bytes, std::size_t& cursor, CancellationToken cancellation) {
    while (cursor < bytes.size() && whitespace(bytes[cursor])) {
        if ((cursor % cancellation_check_interval_bytes) == 0 && cancellation.stop_requested()) return {};
        ++cursor;
    }
    const auto begin = cursor;
    while (cursor < bytes.size() && !whitespace(bytes[cursor])) {
        if ((cursor % cancellation_check_interval_bytes) == 0 && cancellation.stop_requested()) return {};
        ++cursor;
    }
    return TokenLocation{begin, cursor - begin, begin == bytes.size()};
}
bool equal_tokens(std::string_view solution, std::string_view reference, CancellationToken cancellation) {
    if (solution.size() != reference.size()) return false;
    for (std::size_t offset = 0; offset < solution.size(); offset += cancellation_check_interval_bytes) {
        if (cancellation.stop_requested()) return false;
        if (solution.substr(offset, cancellation_check_interval_bytes)
            != reference.substr(offset, cancellation_check_interval_bytes)) return false;
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
    const auto& solution_execution = result.execution.programs.solution;
    const auto& reference_execution = result.execution.programs.reference;
    if (!solution_execution.successful()) { classify(CandidateOutcome::lost); return result; }
    if (!reference_execution.successful()) { classify(CandidateOutcome::inconclusive); return result; }
    std::size_t solution_cursor = 0, reference_cursor = 0;
    for (std::size_t position = 1;; ++position) {
        const auto solution_token = next_token(solution_execution.stdout_bytes, solution_cursor, cancellation);
        const auto reference_token = next_token(reference_execution.stdout_bytes, reference_cursor, cancellation);
        if (!solution_token || !reference_token || cancellation.stop_requested()) {
            result.execution.cancelled = true;
            return result;
        }
        if (solution_token->end_of_output && reference_token->end_of_output) {
            classify(CandidateOutcome::lost);
            return result;
        }
        const auto solution_value = std::string_view(solution_execution.stdout_bytes)
            .substr(solution_token->offset, solution_token->length);
        const auto reference_value = std::string_view(reference_execution.stdout_bytes)
            .substr(reference_token->offset, reference_token->length);
        // A different token count or exact token value preserves the wrong answer.
        const bool tokens_differ = solution_token->end_of_output != reference_token->end_of_output
            || !equal_tokens(solution_value, reference_value, cancellation);
        if (tokens_differ) {
            classify(CandidateOutcome::preserved);
            if (result.outcome) result.disagreement = Disagreement{position, *solution_token, *reference_token};
            return result;
        }
    }
}
}
