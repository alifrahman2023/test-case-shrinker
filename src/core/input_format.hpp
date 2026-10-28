#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace shrink {
enum class Ordering { unrestricted, ascending, descending };

struct InputConstraints {
    std::int64_t min_length;
    std::int64_t max_length;
    std::int64_t min_value;
    std::int64_t max_value;
    Ordering ordering = Ordering::unrestricted;
    bool unique = false;
};

enum class ValidationCode { configuration, integer_spelling, element_count, constraints };
struct ValidationError {
    ValidationCode code;
    std::string message;
};

std::optional<ValidationError> validate_constraints(const InputConstraints& constraints);
std::variant<std::vector<std::int64_t>, ValidationError>
parse_input(std::string_view text, const InputConstraints& constraints);
std::string serialize_input(std::span<const std::int64_t> values);
}
