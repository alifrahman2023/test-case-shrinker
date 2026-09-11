#pragma once

#include <string_view>

namespace shrink {

/// Semantic version of the shrink tool, e.g. "0.1.0".
[[nodiscard]] std::string_view version() noexcept;

/// One-line banner: "shrink <version>".
[[nodiscard]] std::string_view version_banner() noexcept;

}  // namespace shrink
