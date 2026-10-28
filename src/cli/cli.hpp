#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace shrink::cli {
int run(std::span<const std::string_view> arguments, std::ostream& output,
        std::ostream& errors);
}
