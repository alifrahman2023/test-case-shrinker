#pragma once
#include "reduction.hpp"
#include <sys/types.h>

namespace shrink::cli {
// Internal filesystem seam, shared by production and failure-injection tests.
struct SaveFileOperations {
    std::function<ssize_t(int, const void*, std::size_t)> write;
    std::function<int(const char*, const char*)> rename;
};
SaveResult save_atomic(const std::filesystem::path& destination, const SaveRequest& request,
                       const SaveFileOperations& operations = {});
}
