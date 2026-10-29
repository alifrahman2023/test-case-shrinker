#include "coordinator.hpp"

#include <utility>

namespace shrink {
std::optional<ValidationError> validate_execution_configuration(
    const ProgramSpec& solution, const ProgramSpec& reference, const RunLimits& limits) {
    for (const auto* program : {&solution, &reference}) {
        if (program->executable.empty() || program->executable.native().find('\0') != std::string::npos) {
            return ValidationError{ValidationCode::configuration, "both programs require a nonempty executable path without NUL bytes"};
        }
        for (const auto& argument : program->arguments) {
            if (argument.find('\0') != std::string::npos) {
                return ValidationError{ValidationCode::configuration, "program arguments cannot contain NUL bytes"};
            }
        }
    }
    if (limits.solution_timeout.count() <= 0 || limits.reference_timeout.count() <= 0) {
        return ValidationError{ValidationCode::configuration, "program timeouts must be positive"};
    }
    return {};
}
std::variant<PreparedRequest, ValidationError> prepare_request(ReductionRequest request) {
    if (auto error = validate_execution_configuration(request.solution, request.reference, request.limits)) return *error;
    if (request.limits.time_limit && request.limits.time_limit->count() <= 0) {
        return ValidationError{ValidationCode::configuration, "program timeouts and the overall time limit must be positive"};
    }
    auto parsed = parse_input(request.input_text, request.constraints);
    if (auto* error = std::get_if<ValidationError>(&parsed)) return *error;
    auto values = std::get<std::vector<std::int64_t>>(std::move(parsed));
    auto normalized = serialize_input(values);
    return PreparedRequest{std::move(request), std::move(values), std::move(normalized)};
}
}
