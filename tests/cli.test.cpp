#include "cli.hpp"
#include "options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

extern char** environ;

namespace {
namespace fs = std::filesystem;

struct Workspace {
    fs::path previous = fs::current_path();
    fs::path directory;
    Workspace() {
        char pattern[] = "/tmp/shrink-validation-XXXXXX";
        auto* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("cannot create test directory");
        directory = created;
        fs::current_path(directory);
        write("original input.txt", "3 7 -2 7");
        write("result.txt", "previous result\n");
        write("program", "#!/bin/sh\nprintf ran > '" + (directory / "executed").string() + "'\n");
        fs::permissions("program", fs::perms::owner_all);
    }
    ~Workspace() {
        fs::current_path(previous);
        fs::remove_all(directory);
    }
    void write(const fs::path& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary);
        file << text;
        if (!file) throw std::runtime_error("cannot write test fixture");
    }
    std::vector<std::string> arguments() const {
        return {"--input", "original input.txt", "--solution", "./program", "--reference", "./program",
                "--min-length", "0", "--max-length", "9223372036854775807",
                "--min-value", "-9223372036854775808", "--max-value", "9223372036854775807",
                "--output", "result.txt"};
    }
    static std::vector<std::string_view> views(const std::vector<std::string>& arguments) {
        return {arguments.begin(), arguments.end()};
    }
    std::string read(const fs::path& path) const {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), {}};
    }
    std::string reject(const std::vector<std::string>& arguments, int expected = 2) {
        std::vector<std::string> owned_arguments{SHRINK_CLI_PATH};
        owned_arguments.insert(owned_arguments.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& argument : owned_arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        REQUIRE(posix_spawn_file_actions_init(&actions) == 0);
        REQUIRE(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "stdout.txt", O_CREAT | O_TRUNC | O_WRONLY, 0600) == 0);
        REQUIRE(posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "stderr.txt", O_CREAT | O_TRUNC | O_WRONLY, 0600) == 0);
        pid_t child;
        const int launch_error = posix_spawn(&child, SHRINK_CLI_PATH, &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        REQUIRE(launch_error == 0);
        int status;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        REQUIRE(waited == child);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == expected);
        REQUIRE(read("stdout.txt").empty());
        REQUIRE_FALSE(read("stderr.txt").empty());
        REQUIRE_FALSE(fs::exists("executed"));
        REQUIRE(read("result.txt") == "previous result\n");
        return read("stderr.txt");
    }
};
}

TEST_CASE("CLI rejects malformed input before execution or output replacement", "[cli]") {
    Workspace workspace;
    workspace.write("original input.txt", "2 7");
    REQUIRE(workspace.reject(workspace.arguments()).find("fewer elements") != std::string::npos);
}

TEST_CASE("CLI preparation owns normalized input and preserves program argument order", "[cli]") {
    Workspace workspace;
    auto arguments = workspace.arguments();
    arguments.insert(arguments.end(), {"--solution-arg=--algorithm", "--solution-arg", "a b",
                                     "--solution-arg=", "--reference-arg=-x", "--reference-arg", "$(echo literal)",
                                     "--solution-timeout=250ms", "--reference-timeout", "3s", "--time-limit=10s",
                                     "--max-evaluations", "0", "--quiet"});
    auto result = shrink::cli::prepare_invocation(Workspace::views(arguments));
    REQUIRE(std::holds_alternative<shrink::cli::Invocation>(result));
    arguments.clear();
    const auto& invocation = std::get<shrink::cli::Invocation>(result);
    REQUIRE(invocation.core.normalized_input == "3\n7 -2 7\n");
    REQUIRE(invocation.core.request.solution.arguments == std::vector<std::string>{"--algorithm", "a b", ""});
    REQUIRE(invocation.core.request.reference.arguments == std::vector<std::string>{"-x", "$(echo literal)"});
    REQUIRE(invocation.core.request.limits.solution_timeout == std::chrono::milliseconds(250));
    REQUIRE(invocation.core.request.limits.reference_timeout == std::chrono::milliseconds(3000));
    REQUIRE(invocation.core.request.limits.time_limit == std::chrono::milliseconds(10000));
    REQUIRE(invocation.core.request.limits.max_evaluations == 0);
    REQUIRE(invocation.quiet);
    REQUIRE(invocation.input_path == "original input.txt");
    REQUIRE(invocation.output_path == "result.txt");
    REQUIRE_FALSE(fs::exists("executed"));
}

TEST_CASE("CLI preparation defaults output and per-program deadlines without writing", "[cli]") {
    Workspace workspace;
    auto arguments = workspace.arguments();
    arguments.resize(arguments.size() - 2);
    auto result = shrink::cli::prepare_invocation(Workspace::views(arguments));
    REQUIRE(std::holds_alternative<shrink::cli::Invocation>(result));
    const auto& invocation = std::get<shrink::cli::Invocation>(result);
    REQUIRE(invocation.output_path == "reduced.txt");
    REQUIRE(invocation.core.request.limits.solution_timeout == std::chrono::seconds(2));
    REQUIRE(invocation.core.request.limits.reference_timeout == std::chrono::seconds(10));
    REQUIRE_FALSE(invocation.core.request.limits.time_limit);
    REQUIRE_FALSE(invocation.core.request.limits.max_evaluations);
    REQUIRE_FALSE(invocation.quiet);
    REQUIRE_FALSE(fs::exists("reduced.txt"));
}

TEST_CASE("CLI rejects missing unknown repeated and valueless options", "[cli]") {
    Workspace workspace;
    for (std::size_t index = 0; index < 14; index += 2) {
        auto arguments = workspace.arguments();
        auto required = arguments[index];
        arguments.erase(arguments.begin() + index, arguments.begin() + index + 2);
        REQUIRE(workspace.reject(arguments).find(required) != std::string::npos);
    }
    for (auto option : {"--input", "--output", "--solution", "--reference", "--min-length", "--max-length",
                        "--min-value", "--max-value", "--sorted", "--solution-timeout", "--reference-timeout",
                        "--time-limit", "--max-evaluations", "--solution-arg", "--reference-arg"}) {
        auto arguments = workspace.arguments();
        arguments.emplace_back(option);
        workspace.reject(arguments);
    }
    for (auto option : {"--cache", "--diagnostics", "--overwrite", "--crash", "--unknown", "-", "--",
                        "--quiet=yes", "--unique=no", "--help=1", "--version=1"}) {
        auto arguments = workspace.arguments();
        arguments.emplace_back(option);
        workspace.reject(arguments);
    }
    for (auto option : {"--quiet", "--unique", "--sorted=ascending", "--solution-timeout=1s",
                        "--reference-timeout=1s", "--time-limit=1s", "--max-evaluations=1"}) {
        auto arguments = workspace.arguments();
        arguments.insert(arguments.end(), {option, option});
        workspace.reject(arguments);
    }
}

TEST_CASE("CLI rejects invalid numeric bounds durations and evaluation counts", "[cli]") {
    Workspace workspace;
    for (auto value : {"+1", "01", "-0", "1.0", "1e2", "0x10", "1,000", "9223372036854775808", "-9223372036854775809", ""}) {
        for (std::size_t index : {7, 9, 11, 13}) {
            auto arguments = workspace.arguments();
            arguments[index] = value;
            workspace.reject(arguments);
        }
    }
    for (auto value : {"0s", "0ms", "-1s", "1", "1m", "1.5s", "1e2ms", "+1s", "01s", "-0s",
                       "9223372036854775808ms", "9223372036854776s", "18446744073709551616s", "ms", ""}) {
        for (auto option : {"--solution-timeout=", "--reference-timeout=", "--time-limit="}) {
            auto arguments = workspace.arguments();
            arguments.push_back(std::string(option) + value);
            workspace.reject(arguments);
        }
    }
    for (auto value : {"-1", "-0", "+1", "01", "1.0", "1e2", "18446744073709551616", ""}) {
        auto arguments = workspace.arguments();
        arguments.push_back(std::string("--max-evaluations=") + value);
        workspace.reject(arguments);
    }
}

TEST_CASE("CLI accepts equals forms integer extremes and representable limit boundaries", "[cli]") {
    Workspace workspace;
    workspace.write("original input.txt", "2 -9223372036854775808 9223372036854775807");
    auto pairs = workspace.arguments();
    std::vector<std::string> arguments;
    for (std::size_t index = 0; index < pairs.size(); index += 2) {
        arguments.push_back(pairs[index] + '=' + pairs[index + 1]);
    }
    arguments.insert(arguments.end(), {"--solution-timeout=9223372036854775807ms",
                                     "--reference-timeout=9223372036854775s",
                                     "--max-evaluations=18446744073709551615"});
    auto result = shrink::cli::prepare_invocation(Workspace::views(arguments));
    REQUIRE(std::holds_alternative<shrink::cli::Invocation>(result));
    const auto& limits = std::get<shrink::cli::Invocation>(result).core.request.limits;
    REQUIRE(limits.solution_timeout == std::chrono::milliseconds::max());
    REQUIRE(limits.reference_timeout == std::chrono::milliseconds(9223372036854775000));
    REQUIRE(limits.max_evaluations == UINT64_MAX);
}

TEST_CASE("CLI requires equals for leading-minus program arguments and rejects empty paths", "[cli]") {
    Workspace workspace;
    for (auto name : {"--solution-arg", "--reference-arg"}) {
        auto arguments = workspace.arguments();
        arguments.insert(arguments.end(), {name, "--flag"});
        REQUIRE(workspace.reject(arguments).find("equals form") != std::string::npos);
    }
    for (std::size_t index : {1, 3, 5, 15}) {
        auto arguments = workspace.arguments();
        arguments[index].clear();
        workspace.reject(arguments);
    }
}

TEST_CASE("CLI help describes the available configuration without requiring it", "[cli]") {
    std::ostringstream output, errors;
    const std::vector<std::string_view> arguments{"--help"};
    REQUIRE(shrink::cli::run(arguments, output, errors) == 0);
    REQUIRE(errors.str().empty());
    for (auto option : {"--input", "--solution", "--reference", "--min-length", "--max-length", "--min-value",
                        "--max-value", "--output", "--sorted", "--unique", "--solution-arg", "--reference-arg",
                        "--solution-timeout", "--reference-timeout", "--time-limit", "--max-evaluations", "--quiet"}) {
        INFO(option);
        REQUIRE(output.str().find(option) != std::string::npos);
    }
}

TEST_CASE("CLI applies strict input grammar and every declared constraint before execution", "[cli]") {
    Workspace workspace;
    for (auto text : {"", "-1", "+1 2", "01 2", "-0", "9223372036854775808", "1", "0 extra", "1 7 8",
                      "1 +1", "1 01", "1 -0", "1 -01", "1 1.0", "1 1e2", "1 0x10", "1 1,000",
                      "1 9223372036854775808", "1 -9223372036854775809", "9223372036854775807"}) {
        INFO(text);
        workspace.write("original input.txt", text);
        workspace.reject(workspace.arguments());
    }
    workspace.write("original input.txt", "1 1");
    for (auto [index, value] : {std::pair<std::size_t, const char*>{7, "-1"}, {9, "-1"}, {7, "2"},
                              {9, "0"}, {11, "2"}, {13, "0"}}) {
        auto arguments = workspace.arguments();
        arguments[index] = value;
        workspace.reject(arguments);
    }
    for (auto [text, options] : std::vector<std::pair<std::string, std::vector<std::string>>>{
             {"3 1 0 2", {"--sorted", "ascending"}},
             {"3 2 3 1", {"--sorted=descending"}},
             {"3 1 2 1", {"--unique"}},
             {"3 1 1 2", {"--sorted=ascending", "--unique"}},
             {"3 2 1 1", {"--sorted=descending", "--unique"}},
             {"0", {"--sorted=sideways"}}}) {
        workspace.write("original input.txt", text);
        auto arguments = workspace.arguments();
        arguments.insert(arguments.end(), options.begin(), options.end());
        workspace.reject(arguments);
    }
    auto impossible = workspace.arguments();
    impossible[7] = "4";
    impossible[11] = "1";
    impossible[13] = "3";
    impossible.push_back("--unique");
    REQUIRE(workspace.reject(impossible).find("distinct permitted values") != std::string::npos);
}

TEST_CASE("CLI accepts empty singleton and ordered inputs without altering their values", "[cli]") {
    Workspace workspace;
    for (auto [text, normalized, options] : std::vector<std::tuple<std::string, std::string, std::vector<std::string>>>{
             {"0", "0\n\n", {"--sorted=ascending", "--unique"}},
             {"1 -1", "1\n-1\n", {"--sorted=descending", "--unique"}},
             {"3 1 1 2", "3\n1 1 2\n", {"--sorted=ascending"}},
             {"3 2 1 1", "3\n2 1 1\n", {"--sorted=descending"}},
             {"3 3 1 2", "3\n3 1 2\n", {"--unique"}},
             {"3 -1 0 1", "3\n-1 0 1\n", {"--sorted=ascending", "--unique"}}}) {
        workspace.write("original input.txt", text);
        auto arguments = workspace.arguments();
        arguments.insert(arguments.end(), options.begin(), options.end());
        auto result = shrink::cli::prepare_invocation(Workspace::views(arguments));
        REQUIRE(std::holds_alternative<shrink::cli::Invocation>(result));
        REQUIRE(std::get<shrink::cli::Invocation>(result).core.normalized_input == normalized);
    }
}

TEST_CASE("CLI protects input and both executables through path symlink and hard-link aliases", "[cli]") {
    Workspace workspace;
    workspace.write("reference program", workspace.read("program"));
    fs::permissions("reference program", fs::perms::owner_all);
    auto base = workspace.arguments();
    base[5] = "reference program";
    for (auto protected_path : {"original input.txt", "program", "reference program"}) {
        const auto original = workspace.read(protected_path);
        fs::create_hard_link(protected_path, "hard alias");
        fs::create_symlink(protected_path, "symbolic alias");
        for (auto destination : {std::string(protected_path), std::string("hard alias"), std::string("symbolic alias"),
                                 std::string("./") + protected_path}) {
            auto arguments = base;
            arguments.back() = destination;
            workspace.reject(arguments);
            REQUIRE(workspace.read(protected_path) == original);
        }
        // Protected paths may themselves be symbolic aliases.
        auto arguments = base;
        auto index = protected_path == std::string("original input.txt") ? 1
                   : protected_path == std::string("program") ? 3 : 5;
        arguments[index] = "symbolic alias";
        arguments.back() = "hard alias";
        workspace.reject(arguments);
        fs::remove("hard alias");
        fs::remove("symbolic alias");
    }
    fs::create_directory_symlink(workspace.directory, "parent alias");
    auto arguments = base;
    arguments.back() = "parent alias/original input.txt";
    workspace.reject(arguments);
    workspace.write("reduced.txt", "protected default input");
    arguments = base;
    arguments[1] = "reduced.txt";
    arguments.resize(arguments.size() - 2);
    workspace.reject(arguments);
    REQUIRE(workspace.read("reduced.txt") == "protected default input");
}

TEST_CASE("CLI permits input executable and parent symlinks with a safe regular output", "[cli]") {
    Workspace workspace;
    fs::create_symlink("original input.txt", "input alias");
    fs::create_symlink("program", "program alias");
    fs::create_directory_symlink(workspace.directory, "parent alias");
    for (auto output : {"parent alias/new result.txt", "result.txt"}) {
        auto arguments = workspace.arguments();
        arguments[1] = "input alias";
        arguments[3] = "./program alias";
        arguments[5] = "./program alias";
        arguments.back() = output;
        auto result = shrink::cli::prepare_invocation(Workspace::views(arguments));
        REQUIRE(std::holds_alternative<shrink::cli::Invocation>(result));
        REQUIRE_FALSE(fs::exists("executed"));
        REQUIRE(workspace.read("result.txt") == "previous result\n");
        REQUIRE_FALSE(fs::exists("new result.txt"));
    }
}

TEST_CASE("CLI rejects special input output dangling output symlinks and missing parents", "[cli]") {
    Workspace workspace;
    fs::create_directory("directory");
    REQUIRE(mkfifo("fifo", 0600) == 0);
    fs::create_symlink("missing file", "dangling link");
    for (auto path : {"directory", "fifo", "/dev/null"}) {
        auto arguments = workspace.arguments();
        arguments[1] = path;
        workspace.reject(arguments);
    }
    for (auto path : {"directory", "fifo", "/dev/null", "dangling link", "missing/result.txt", "program/result.txt", "program/sub/result.txt"}) {
        auto arguments = workspace.arguments();
        arguments.back() = path;
        workspace.reject(arguments);
    }
}

TEST_CASE("Input-read failures are structured operational errors with unchanged results", "[cli]") {
    Workspace workspace;
    fs::permissions("original input.txt", fs::perms::none);
    auto result = shrink::cli::prepare_invocation(Workspace::views(workspace.arguments()));
    REQUIRE(std::holds_alternative<shrink::cli::PreparationError>(result));
    const auto& error = std::get<shrink::cli::PreparationError>(result);
    REQUIRE(error.kind == shrink::cli::PreparationErrorKind::file_io);
    REQUIRE(error.operation == "open input");
    REQUIRE(error.path == "original input.txt");
    REQUIRE(error.system_error == std::errc::permission_denied);
    workspace.reject(workspace.arguments(), 1);
    fs::permissions("original input.txt", fs::perms::owner_all);
    auto arguments = workspace.arguments();
    arguments[1] = "missing input";
    workspace.reject(arguments, 1);
}

TEST_CASE("CLI leaves an existing default output unchanged on rejection", "[cli]") {
    Workspace workspace;
    workspace.write("original input.txt", "not an array");
    workspace.write("reduced.txt", "previous default result\n");
    auto arguments = workspace.arguments();
    arguments.resize(arguments.size() - 2);
    workspace.reject(arguments);
    REQUIRE(workspace.read("reduced.txt") == "previous default result\n");
}
