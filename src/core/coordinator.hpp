#pragma once

#include "input_format.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace shrink {
struct ProgramSpec {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
};

struct RunLimits {
    std::chrono::milliseconds solution_timeout{2000};
    std::chrono::milliseconds reference_timeout{10000};
    // Optional positive overall elapsed budget. Starts after preparation and
    // uses the runner's sleep-inclusive monotonic clock. Includes baseline,
    // search and checkpoints; cleanup/final saving may extend beyond expiry.
    std::optional<std::chrono::milliseconds> time_limit;
    std::optional<std::uint64_t> max_evaluations;
};

struct ReductionRequest {
    std::string input_text;
    InputConstraints constraints;
    ProgramSpec solution;
    ProgramSpec reference;
    RunLimits limits;
};

struct PreparedRequest {
    ReductionRequest request;
    std::vector<std::int64_t> values;
    std::string normalized_input;
};

// Shared program-path/argument and per-program deadline validation. Overall
// reduction budgets are validated by prepare_request, not candidate execution.
std::optional<ValidationError> validate_execution_configuration(
    const ProgramSpec& solution, const ProgramSpec& reference, const RunLimits& limits);

std::variant<PreparedRequest, ValidationError> prepare_request(ReductionRequest request);
}
