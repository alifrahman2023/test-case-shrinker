#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace shrink {
// Data integers are distinct from the derived array length header.
struct IntegerNode {
    std::int64_t value;
};

class ArrayNode {
public:
    std::size_t size() const { return elements_.size(); }
    bool empty() const { return elements_.empty(); }
    const IntegerNode& operator[](std::size_t index) const { return elements_[index]; }
    const IntegerNode& back() const { return elements_.back(); }
    std::span<const IntegerNode> elements() const { return elements_; }
    // Construction permits only ordered integer children. There is no mutable
    // child access or independently editable count header.
    void append(IntegerNode integer) { elements_.push_back(integer); }
private:
    std::vector<IntegerNode> elements_;
};

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
std::variant<ArrayNode, ValidationError>
parse_input(std::string_view text, const InputConstraints& constraints);
std::string serialize_input(const ArrayNode& array);
}
