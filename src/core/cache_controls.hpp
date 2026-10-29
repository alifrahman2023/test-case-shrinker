#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace shrink::detail {
// Internal search controls for equivalence/collision checks, never CLI options.
struct CacheControls {
    bool enabled = true;
    std::size_t max_entries = 4096;
    std::size_t max_text_bytes = 8 * 1024 * 1024;
    std::optional<std::uint64_t> forced_fingerprint;
};
}
