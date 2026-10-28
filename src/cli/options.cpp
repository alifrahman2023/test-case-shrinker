#include "options.hpp"
#include "integer.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace shrink::cli {
namespace {
namespace fs = std::filesystem;
struct Options {
    ReductionRequest request{"", {0, 0, 0, 0}, {}, {}, {}};
    fs::path input;
    fs::path output = "reduced.txt";
    bool quiet = false;
};

PreparationError invalid(std::string message, fs::path path = {}) {
    return {PreparationErrorKind::invalid_configuration, "validate configuration", std::move(path), std::move(message), {}};
}
PreparationError io_error(std::string operation, const fs::path& path, std::error_code error) {
    return {PreparationErrorKind::file_io, std::move(operation), path, error.message(), error};
}

std::optional<std::chrono::milliseconds> parse_duration(std::string_view text) {
    std::int64_t multiplier;
    if (text.ends_with("ms")) {
        multiplier = 1;
        text.remove_suffix(2);
    } else if (text.ends_with('s')) {
        multiplier = 1000;
        text.remove_suffix(1);
    } else {
        return std::nullopt;
    }
    auto count = parse_decimal<std::int64_t>(text);
    if (!count || *count <= 0 || *count > std::chrono::milliseconds::max().count() / multiplier) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(*count * multiplier);
}

std::variant<Options, PreparationError> parse_options(std::span<const std::string_view> arguments) {
    Options options;
    std::unordered_set<std::string_view> seen;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto argument = arguments[index];
        auto equals = argument.find('=');
        auto name = argument.substr(0, equals);
        const bool program_argument = name == "--solution-arg" || name == "--reference-arg";
        if (!seen.insert(name).second && !program_argument) {
            return invalid("repeated option: " + std::string(name));
        }
        if (name == "--quiet" || name == "--unique") {
            if (equals != std::string_view::npos) return invalid(std::string(name) + " does not take a value");
            if (name == "--quiet") options.quiet = true;
            else options.request.constraints.unique = true;
            continue;
        }
        constexpr std::array value_options{
            "--input", "--output", "--solution", "--reference", "--min-length", "--max-length",
            "--min-value", "--max-value", "--sorted", "--solution-arg", "--reference-arg",
            "--solution-timeout", "--reference-timeout", "--time-limit", "--max-evaluations"};
        bool recognized = false;
        for (auto option : value_options) recognized |= name == option;
        if (!recognized) return invalid("unknown option: " + std::string(name));
        std::string_view value;
        if (equals != std::string_view::npos) {
            value = argument.substr(equals + 1);
        } else {
            if (++index == arguments.size()) return invalid("missing value for " + std::string(name));
            value = arguments[index];
            if (program_argument && value.starts_with('-')) {
                return invalid(std::string(name) + " requires the equals form for an argument beginning with '-'");
            }
        }
        if (value.find('\0') != std::string_view::npos) return invalid("option values cannot contain NUL bytes");
        if (name == "--input") options.input = value;
        else if (name == "--output") options.output = value;
        else if (name == "--solution") options.request.solution.executable = value;
        else if (name == "--reference") options.request.reference.executable = value;
        else if (name == "--solution-arg") options.request.solution.arguments.emplace_back(value);
        else if (name == "--reference-arg") options.request.reference.arguments.emplace_back(value);
        else if (name == "--sorted") {
            if (value == "ascending") options.request.constraints.ordering = Ordering::ascending;
            else if (value == "descending") options.request.constraints.ordering = Ordering::descending;
            else return invalid("--sorted must be ascending or descending");
        } else if (name == "--max-evaluations") {
            auto count = parse_decimal<std::uint64_t>(value);
            if (!count) return invalid("--max-evaluations must be a nonnegative unsigned-64-bit decimal integer");
            options.request.limits.max_evaluations = *count;
        } else if (name == "--solution-timeout" || name == "--reference-timeout" || name == "--time-limit") {
            auto duration = parse_duration(value);
            if (!duration) return invalid(std::string(name) + " must be a positive representable whole duration ending in ms or s");
            if (name == "--solution-timeout") options.request.limits.solution_timeout = *duration;
            else if (name == "--reference-timeout") options.request.limits.reference_timeout = *duration;
            else options.request.limits.time_limit = *duration;
        } else {
            auto number = parse_decimal<std::int64_t>(value);
            if (!number) return invalid(std::string(name) + " must be a signed-64-bit decimal integer");
            if (name == "--min-length") options.request.constraints.min_length = *number;
            else if (name == "--max-length") options.request.constraints.max_length = *number;
            else if (name == "--min-value") options.request.constraints.min_value = *number;
            else options.request.constraints.max_value = *number;
        }
    }
    for (auto name : {"--input", "--solution", "--reference", "--min-length", "--max-length", "--min-value", "--max-value"}) {
        if (!seen.contains(name)) return invalid("required option missing: " + std::string(name));
    }
    if (options.input.empty() || options.output.empty()) return invalid("input and output paths must be nonempty");
    return options;
}

std::optional<PreparationError> check_files(const Options& options) {
    std::error_code error;
    auto input_status = fs::status(options.input, error);
    if (error) return io_error("inspect input", options.input, error);
    if (!fs::is_regular_file(input_status)) return invalid("input must be a regular file", options.input);
    auto parent = options.output.parent_path();
    if (parent.empty()) parent = ".";
    auto parent_status = fs::status(parent, error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory
        || (!error && !fs::is_directory(parent_status))) {
        return invalid("output parent must be an existing directory", parent);
    }
    if (error) return io_error("inspect output parent", parent, error);
    auto output_status = fs::symlink_status(options.output, error);
    if (error == std::errc::no_such_file_or_directory) return std::nullopt;
    if (error) return io_error("inspect output", options.output, error);
    if (!fs::exists(output_status)) return std::nullopt;
    if (!fs::is_regular_file(output_status)) return invalid("output must be a regular file and cannot be a symbolic link", options.output);
    for (const auto& protected_path : {options.input, options.request.solution.executable, options.request.reference.executable}) {
        auto status = fs::status(protected_path, error);
        if (error == std::errc::no_such_file_or_directory) continue;
        if (error) return io_error("inspect protected file", protected_path, error);
        if (fs::exists(status) && fs::equivalent(options.output, protected_path, error)) {
            return invalid("output aliases the original input or an executable", options.output);
        }
        if (error) return io_error("compare output file identity", protected_path, error);
    }
    return std::nullopt;
}

std::variant<std::string, PreparationError> read_input(const fs::path& path) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) return io_error("open input", path, {errno, std::generic_category()});
    struct Descriptor {
        int value;
        ~Descriptor() { close(value); }
    } owned{descriptor};
    struct stat status{};
    if (fstat(descriptor, &status) < 0) return io_error("inspect opened input", path, {errno, std::generic_category()});
    if (!S_ISREG(status.st_mode)) return invalid("opened input must be a regular file", path);
    std::string text;
    std::array<char, 65536> buffer{};
    for (;;) {
        const auto count = read(descriptor, buffer.data(), buffer.size());
        if (count == 0) return text;
        if (count < 0) {
            if (errno == EINTR) continue;
            return io_error("read input", path, {errno, std::generic_category()});
        }
        text.append(buffer.data(), static_cast<std::size_t>(count));
    }
}
}

std::variant<Invocation, PreparationError>
prepare_invocation(std::span<const std::string_view> arguments) {
    auto parsed = parse_options(arguments);
    if (auto* error = std::get_if<PreparationError>(&parsed)) return *error;
    auto options = std::get<Options>(std::move(parsed));
    if (auto error = check_files(options)) return *error;
    auto input = read_input(options.input);
    if (auto* error = std::get_if<PreparationError>(&input)) return *error;
    options.request.input_text = std::get<std::string>(std::move(input));
    auto prepared = prepare_request(std::move(options.request));
    if (auto* error = std::get_if<ValidationError>(&prepared)) return invalid(error->message);
    return Invocation{std::get<PreparedRequest>(std::move(prepared)), std::move(options.input),
                      std::move(options.output), options.quiet};
}
}
