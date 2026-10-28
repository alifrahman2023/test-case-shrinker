#pragma once
#include "runner.hpp"

namespace shrink {
enum class CandidateOutcome { preserved, lost, inconclusive };
struct TokenLocation {
    std::size_t offset;
    std::size_t length; // zero at end-of-output
    bool end_of_output;
};
struct Disagreement {
    std::size_t token_position; // one-based
    TokenLocation solution;
    TokenLocation reference;
};
struct Evaluation {
    ExecutionPair execution;
    // Absent on operational error or cancellation; never accept partial output.
    std::optional<CandidateOutcome> outcome;
    std::optional<Disagreement> disagreement;
};
Evaluation evaluate_candidate(Runner& runner, std::string_view normalized_input,
                              const ProgramSpec& solution, const ProgramSpec& reference,
                              const RunLimits& limits, CancellationToken cancellation = {});
}
