#include "saving.hpp"
#include <cerrno>
#include <cstdio>
#include <unistd.h>

namespace shrink::cli {
namespace {
struct TemporaryFile {
    std::string path;
    int descriptor = -1;
    ~TemporaryFile() {
        if (descriptor >= 0) close(descriptor);
        if (!path.empty()) unlink(path.c_str());
    }
};
}
SaveResult save_atomic(const std::filesystem::path& destination, const SaveRequest& request,
                       const SaveFileOperations& operations) {
    auto failure = [&](SaveErrorCode code, const char* operation) -> SaveResult {
        return SaveError{code, operation, destination, std::error_code(errno, std::generic_category())};
    };
    auto parent = destination.parent_path();
    if (parent.empty()) parent = ".";
    TemporaryFile temporary{(parent / ".shrink-XXXXXX").string()};
    temporary.descriptor = mkstemp(temporary.path.data());
    if (temporary.descriptor < 0) {
        temporary.path.clear();
        return failure(SaveErrorCode::create_temporary, "create temporary result");
    }
    std::size_t offset = 0;
    while (offset < request.normalized_input.size()) {
        const auto count = std::min<std::size_t>(request.normalized_input.size() - offset, 64 * 1024);
        const auto* bytes = request.normalized_input.data() + offset;
        const auto written = operations.write ? operations.write(temporary.descriptor, bytes, count)
                                              : ::write(temporary.descriptor, bytes, count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            if (written == 0) errno = EIO;
            return failure(SaveErrorCode::write_temporary, "write temporary result");
        }
        offset += static_cast<std::size_t>(written);
    }
    const int descriptor = temporary.descriptor;
    temporary.descriptor = -1; // Never blindly retry close after an uncertain error.
    if (close(descriptor) < 0) return failure(SaveErrorCode::close_temporary, "close temporary result");
    const int replaced = operations.rename ? operations.rename(temporary.path.c_str(), destination.c_str())
                                           : ::rename(temporary.path.c_str(), destination.c_str());
    if (replaced < 0) return failure(SaveErrorCode::replace, "replace result");
    temporary.path.clear();
    return {};
}
}
