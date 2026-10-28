#pragma once

#include "coordinator.hpp"

#include <span>
#include <string_view>
#include <system_error>
#include <variant>

namespace shrink::cli {
enum class PreparationErrorKind { invalid_configuration, file_io };
struct PreparationError {
    PreparationErrorKind kind;
    std::string operation;
    std::filesystem::path path;
    std::string message;
    std::error_code system_error;
};

struct Invocation {
    PreparedRequest core;
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    bool quiet;
};

// CLI preparation performs no program execution or result-file writes.
std::variant<Invocation, PreparationError>
prepare_invocation(std::span<const std::string_view> arguments);
}
