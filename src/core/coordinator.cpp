#include "coordinator.hpp"

#include <utility>

namespace shrink {
std::variant<PreparedRequest, ValidationError> prepare_request(ReductionRequest request) {
    for (const auto* program : {&request.solution, &request.reference}) {
        if (program->executable.empty() || program->executable.native().find('\0') != std::string::npos) {
            return ValidationError{ValidationCode::configuration, "both programs require a nonempty executable path without NUL bytes"};
        }
        for (const auto& argument : program->arguments) {
            if (argument.find('\0') != std::string::npos) {
                return ValidationError{ValidationCode::configuration, "program arguments cannot contain NUL bytes"};
            }
        }
    }
    if (request.limits.solution_timeout.count() <= 0 || request.limits.reference_timeout.count() <= 0
        || (request.limits.time_limit && request.limits.time_limit->count() <= 0)) {
        return ValidationError{ValidationCode::configuration, "program timeouts and the overall time limit must be positive"};
    }
    auto parsed = parse_input(request.input_text, request.constraints);
    if (auto* error = std::get_if<ValidationError>(&parsed)) return *error;
    auto values = std::get<std::vector<std::int64_t>>(std::move(parsed));
    auto normalized = serialize_input(values);
    return PreparedRequest{std::move(request), std::move(values), std::move(normalized)};
}
}
