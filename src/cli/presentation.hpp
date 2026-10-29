#pragma once
#include "reduction.hpp"
#include <iosfwd>

namespace shrink::cli {
int present_result(const ReductionResult& result, const std::filesystem::path& destination,
                   std::ostream& output, std::ostream& errors);
}
