#pragma once

#include <charconv>
#include <optional>
#include <string_view>
#include <type_traits>

namespace shrink {
// Strict decimal spelling shared by input text and numeric CLI options.
template <typename Integer>
std::optional<Integer> parse_decimal(std::string_view text) {
    if (text.empty()) return std::nullopt;
    auto digits = text;
    if (digits.front() == '-') {
        if constexpr (!std::is_signed_v<Integer>) return std::nullopt;
        digits.remove_prefix(1);
        if (digits.empty() || digits.front() == '0') return std::nullopt;
    }
    if (digits.size() > 1 && digits.front() == '0') return std::nullopt;
    for (char digit : digits) {
        if (digit < '0' || digit > '9') return std::nullopt;
    }
    Integer value{};
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}
}
