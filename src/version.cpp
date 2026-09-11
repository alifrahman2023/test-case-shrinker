#include <shrink/version.hpp>

#ifndef SHRINK_VERSION
#define SHRINK_VERSION "0.0.0-unknown"
#endif

namespace shrink {

std::string_view version() noexcept {
  return SHRINK_VERSION;
}

std::string_view version_banner() noexcept {
  return "shrink " SHRINK_VERSION;
}

}  // namespace shrink
