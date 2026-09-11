#pragma once

#include <string>
#include <utility>

namespace shrink {

/// Outcome of a parse. A bad spec or an input that does not match its spec are expected
/// user errors, so they are values. Exceptions stay reserved for programmer errors.
template <typename T>
struct Parsed {
  T value{};
  std::string error;  ///< empty exactly when the parse succeeded

  [[nodiscard]] bool ok() const noexcept { return error.empty(); }

  static Parsed success(T v) { return Parsed{std::move(v), std::string{}}; }
  static Parsed failure(std::string message) { return Parsed{T{}, std::move(message)}; }
};

}  // namespace shrink
