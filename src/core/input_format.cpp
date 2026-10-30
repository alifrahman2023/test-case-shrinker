#include "input_format.hpp"
#include "integer.hpp"

#include <unordered_set>

namespace shrink {
namespace {
std::optional<std::string_view> next_token(std::string_view& text) {
    constexpr auto whitespace = " \t\r\n\f\v";
    auto start = text.find_first_not_of(whitespace);
    if (start == std::string_view::npos) return std::nullopt;
    text.remove_prefix(start);
    auto end = text.find_first_of(whitespace);
    auto token = text.substr(0, end);
    text.remove_prefix(token.size());
    return token;
}
}

std::optional<ValidationError> validate_constraints(const InputConstraints& constraints) {
    if (constraints.min_length < 0 || constraints.max_length < constraints.min_length) {
        return ValidationError{ValidationCode::configuration, "length bounds must satisfy 0 <= minimum <= maximum"};
    }
    if (constraints.max_value < constraints.min_value) {
        return ValidationError{ValidationCode::configuration, "value bounds must satisfy minimum <= maximum"};
    }
    if (constraints.ordering != Ordering::unrestricted && constraints.ordering != Ordering::ascending
        && constraints.ordering != Ordering::descending) {
        return ValidationError{ValidationCode::configuration, "unknown input ordering"};
    }
    // Compare minimum - 1 to the interval width, avoiding a 2^64 interval size.
    const auto width = static_cast<std::uint64_t>(constraints.max_value)
                     - static_cast<std::uint64_t>(constraints.min_value);
    if (constraints.unique && constraints.min_length > 0
        && static_cast<std::uint64_t>(constraints.min_length - 1) > width) {
        return ValidationError{ValidationCode::configuration, "minimum length exceeds the number of distinct permitted values"};
    }
    return std::nullopt;
}

std::variant<ArrayNode, ValidationError>
parse_input(std::string_view text, const InputConstraints& constraints) {
    if (auto error = validate_constraints(constraints)) return *error;
    auto header = next_token(text);
    auto length = header ? parse_decimal<std::int64_t>(*header) : std::nullopt;
    if (!length || *length < 0) {
        return ValidationError{ValidationCode::integer_spelling, "input length must be a nonnegative signed-64-bit decimal integer"};
    }
    if (*length < constraints.min_length || *length > constraints.max_length) {
        return ValidationError{ValidationCode::constraints, "input length is outside the declared bounds"};
    }
    ArrayNode values;
    std::unordered_set<std::int64_t> seen;
    for (std::int64_t index = 0; index < *length; ++index) {
        auto token = next_token(text);
        if (!token) return ValidationError{ValidationCode::element_count, "input has fewer elements than its length header"};
        auto value = parse_decimal<std::int64_t>(*token);
        if (!value) {
            return ValidationError{ValidationCode::integer_spelling, "element " + std::to_string(index + 1) + " is not a signed-64-bit decimal integer"};
        }
        if (*value < constraints.min_value || *value > constraints.max_value) {
            return ValidationError{ValidationCode::constraints, "element " + std::to_string(index + 1) + " is outside the declared value bounds"};
        }
        if (!values.empty()
            && ((constraints.ordering == Ordering::ascending && *value < values.back().value)
                || (constraints.ordering == Ordering::descending && *value > values.back().value))) {
            return ValidationError{ValidationCode::constraints, "input does not satisfy the declared ordering"};
        }
        if (constraints.unique && !seen.insert(*value).second) {
            return ValidationError{ValidationCode::constraints, "input contains duplicate values"};
        }
        values.append(IntegerNode{*value});
    }
    if (next_token(text)) return ValidationError{ValidationCode::element_count, "input has extra data after its declared elements"};
    return values;
}

std::string serialize_input(const ArrayNode& values) {
    auto text = std::to_string(values.size()) + '\n';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) text += ' ';
        text += std::to_string(values[index].value);
    }
    text += '\n';
    return text;
}
}
