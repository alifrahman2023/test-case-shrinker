#include "version.hpp"
#include "shrink/version.hpp"

namespace shrink {
std::string_view version() noexcept {
    return SHRINK_VERSION;
}
}
