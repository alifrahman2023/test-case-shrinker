#include "cli.hpp"
#include "saving.hpp"
#include "options.hpp"
#include "runner_backend.hpp"

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
#include <thread>
#include <signal.h>
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

namespace {
std::vector<std::string> fixture_arguments(const Workspace& workspace, const std::vector<std::string>& solution,
                                          const std::vector<std::string>& reference) {
    auto arguments = workspace.arguments();
    arguments[3] = SHRINK_PROGRAM_FIXTURE; arguments[5] = SHRINK_PROGRAM_FIXTURE;
    arguments.push_back("--max-evaluations=0");
    for (const auto& argument : solution) arguments.push_back("--solution-arg=" + argument);
    for (const auto& argument : reference) arguments.push_back("--reference-arg=" + argument);
    return arguments;
}
}
TEST_CASE("CLI verifies and replaces a zero-budget baseline with exact-token evidence", "[cli-baseline]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"text", "1"}, {"text", "1.0"});
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 4);
    REQUIRE(output.str().empty());
    REQUIRE(workspace.read("result.txt") == "3\n7 -2 7\n");
    REQUIRE(errors.str().find("Baseline: both programs succeeded; first differing token: 1") != std::string::npos);
    REQUIRE(errors.str().find("Final: both programs succeeded; first differing token: 1") != std::string::npos);
    REQUIRE(errors.str().find("\"1.0\"") != std::string::npos);
    REQUIRE(errors.str().find("No reduction round began") != std::string::npos);
    REQUIRE(errors.str().find("evaluation limit") != std::string::npos);
}
TEST_CASE("CLI final-save failure emits complete normalized recovery and reports file context", "[cli-baseline]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"text", "1"}, {"text", "2"});
    shrink::cli::InvocationServices services;
    unsigned calls = 0;
    services.save = [&](const fs::path& path, const shrink::SaveRequest&) -> shrink::SaveResult {
        ++calls;
        return shrink::SaveError{shrink::SaveErrorCode::replace, "replace result", path,
                                 std::make_error_code(std::errc::permission_denied)};
    };
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors, services) == 1);
    REQUIRE(calls == 2);
    REQUIRE(output.str() == "3\n7 -2 7\n");
    REQUIRE(workspace.read("result.txt") == "previous result\n");
    REQUIRE(errors.str().find("result.txt") != std::string::npos);
    REQUIRE(errors.str().find("Permission denied") != std::string::npos);
    REQUIRE(errors.str().find("Final save: failed") != std::string::npos);
}

TEST_CASE("CLI explains unsuccessful solution and reference baseline executions", "[cli-baseline]") {
    Workspace workspace;
    for (const auto& [solution, reference, fact] : std::vector<std::tuple<std::vector<std::string>, std::vector<std::string>, std::string>>{
        {{"exit", "7"}, {"text", "2"}, "solution: exit 7"},
        {{"exit", "127"}, {"text", "2"}, "solution: exit 127"},
        {{"text", "1"}, {"exit", "127"}, "reference: exit 127"},
        {{"signal", "15"}, {"text", "2"}, "signal 15"},
        {{"emit", "8388609", "0"}, {"text", "2"}, "stdout overflow"}}) {
        auto arguments = fixture_arguments(workspace, solution, reference);
        std::ostringstream output, errors;
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 3);
        REQUIRE(output.str().empty());
        REQUIRE(errors.str().find(fact) != std::string::npos);
        REQUIRE(errors.str().find("No verified result") != std::string::npos);
        REQUIRE(workspace.read("result.txt") == "previous result\n");
    }
}
TEST_CASE("CLI bounds escaped unsuccessful stderr and distinguishes retention truncation", "[cli-baseline]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"fail-stderr", "70000"}, {"text", "2"});
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 3);
    REQUIRE(errors.str().find(std::string(4096, 'e')) != std::string::npos);
    REQUIRE(errors.str().find(std::string(4097, 'e')) == std::string::npos);
    REQUIRE(errors.str().find("presentation truncated") != std::string::npos);
    REQUIRE(errors.str().find("retention truncated at 64 KiB") != std::string::npos);
}
TEST_CASE("CLI runs only the normalized baseline once and resume verifies it afresh", "[cli-baseline]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"record-input", "solution-input", "1"}, {"record-input", "reference-input", "2"});
    arguments.push_back("--quiet");
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 4);
    REQUIRE(output.str().empty());
    REQUIRE(workspace.read("solution-input") == "3\n7 -2 7\n");
    REQUIRE(workspace.read("reference-input") == "3\n7 -2 7\n");
    REQUIRE(errors.str().find("Baseline:") != std::string::npos);
    REQUIRE(errors.str().find("Final:") != std::string::npos);
    arguments[1] = "result.txt";
    arguments[15] = "resumed.txt";
    std::ostringstream resumed_output, resumed_errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), resumed_output, resumed_errors) == 4);
    REQUIRE(workspace.read("resumed.txt") == "3\n7 -2 7\n");
    REQUIRE(workspace.read("solution-input") == "3\n7 -2 7\n3\n7 -2 7\n");
    REQUIRE(workspace.read("reference-input") == "3\n7 -2 7\n3\n7 -2 7\n");
    arguments[15] = "result.txt";
    std::ostringstream alias_output, alias_errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), alias_output, alias_errors) == 2);
}
TEST_CASE("CLI Ctrl-C stops baseline children without publishing unverified input", "[cli-baseline]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"rendezvous", "ready", "missing-peer"}, {"rendezvous", "reference-ready", "missing-peer"});
    std::vector<std::string> owned{SHRINK_CLI_PATH}; owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv; for (auto& argument : owned) argv.push_back(argument.data()); argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    REQUIRE(posix_spawn_file_actions_init(&actions) == 0);
    REQUIRE(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "stdout.txt", O_CREAT | O_TRUNC | O_WRONLY, 0600) == 0);
    REQUIRE(posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "stderr.txt", O_CREAT | O_TRUNC | O_WRONLY, 0600) == 0);
    pid_t child;
    const int launched = posix_spawn(&child, SHRINK_CLI_PATH, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    REQUIRE(launched == 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists("ready") && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool ready = fs::exists("ready");
    kill(child, ready ? SIGINT : SIGKILL);
    int status = 0;
    pid_t waited = 0;
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((waited = waitpid(child, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < stop_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (waited == 0) { kill(child, SIGKILL); waitpid(child, &status, 0); }
    REQUIRE(ready);
    REQUIRE(waited == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 130);
    REQUIRE(workspace.read("stdout.txt").empty());
    REQUIRE(workspace.read("result.txt") == "previous result\n");
    REQUIRE(workspace.read("stderr.txt").find("caller cancellation") != std::string::npos);
}
TEST_CASE("CLI stdout recovery failure remains an operational error", "[cli-baseline]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"text", "1"}, {"text", "2"});
    shrink::cli::InvocationServices services;
    services.save = [](const fs::path& path, const shrink::SaveRequest&) -> shrink::SaveResult {
        return shrink::SaveError{shrink::SaveErrorCode::write_temporary, "write temporary result", path,
                                 std::make_error_code(std::errc::no_space_on_device)};
    };
    for (bool throws : {false, true}) {
        std::ostringstream output, errors;
        output.setstate(std::ios::badbit);
        if (throws) { try { output.exceptions(std::ios::badbit); } catch (...) {} }
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors, services) == 1);
        REQUIRE(errors.str().find("stdout recovery failed") != std::string::npos);
        REQUIRE(errors.str().find("write temporary result") != std::string::npos);
        REQUIRE(workspace.read("result.txt") == "previous result\n");
    }
}
TEST_CASE("CLI retains an initial saving error even after successful final replacement", "[cli-baseline]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"text", "1"}, {"text", "2"});
    shrink::cli::InvocationServices services;
    std::vector<shrink::SaveKind> saves;
    services.save = [&](const fs::path& path, const shrink::SaveRequest& save) -> shrink::SaveResult {
        saves.push_back(save.kind);
        if (save.kind == shrink::SaveKind::initial)
            return shrink::SaveError{shrink::SaveErrorCode::write_temporary, "write temporary result", path,
                                     std::make_error_code(std::errc::no_space_on_device)};
        return shrink::cli::save_atomic(path, save);
    };
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors, services) == 1);
    REQUIRE(saves == std::vector{shrink::SaveKind::initial, shrink::SaveKind::final});
    REQUIRE(workspace.read("result.txt") == "3\n7 -2 7\n");
    REQUIRE(output.str().empty());
    REQUIRE(errors.str().find("Final save: succeeded") != std::string::npos);
    REQUIRE(errors.str().find("Save error (initial)") != std::string::npos);
}
TEST_CASE("CLI default output is replaced only for a verified baseline", "[cli-baseline]") {
    Workspace workspace;
    workspace.write("reduced.txt", "previous default\n");
    auto arguments = fixture_arguments(workspace, {"text", "1"}, {"text", "1.0"});
    arguments.erase(arguments.begin() + 14, arguments.begin() + 16);
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 4);
    REQUIRE(workspace.read("reduced.txt") == "3\n7 -2 7\n");
    // A saved default input cannot resume to the same default destination.
    arguments[1] = "reduced.txt";
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 2);
}
TEST_CASE("CLI agreement leaves an existing output unchanged and a missing output absent", "[cli-baseline]") {
    Workspace workspace;
    for (bool existing : {true, false}) {
        auto arguments = fixture_arguments(workspace, {"text", "1 \t2\n"}, {"text", "1\n2 "});
        if (!existing) arguments[15] = "new-result";
        std::ostringstream output, errors;
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 3);
        REQUIRE(output.str().empty());
        REQUIRE(errors.str().find("No wrong-answer disagreement found for this input.") != std::string::npos);
        REQUIRE(errors.str().find(existing ? "was left unchanged" : "was not created") != std::string::npos);
        REQUIRE(workspace.read("result.txt") == "previous result\n");
        REQUIRE_FALSE(fs::exists("new-result"));
    }
}
TEST_CASE("CLI token previews escape bytes, mark truncation, and expose end of output", "[cli-baseline]") {
    Workspace workspace;
    for (const auto& [solution, reference, preview] : std::vector<std::tuple<std::string, std::string, std::string>>{
        {"same \x01\"\\", "same other", "\\x01\\\"\\\\"},
        {"same", "same more", "end of output"},
        {std::string(100, 'x'), "different", "[presentation truncated]"}}) {
        auto arguments = fixture_arguments(workspace, {"text", solution}, {"text", reference});
        std::ostringstream output, errors;
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 4);
        REQUIRE(errors.str().find(preview) != std::string::npos);
        REQUIRE(errors.str().find(std::string(81, 'x')) == std::string::npos);
        REQUIRE(errors.str().find('\x01') == std::string::npos);
        REQUIRE(output.str().empty());
    }
}
TEST_CASE("CLI reports timeout, peer stop, overall expiry, and launch errors distinctly", "[cli-baseline]") {
    Workspace workspace;
    struct Case {
        const char* name;
        std::vector<std::string> solution;
        std::vector<std::string> reference;
        std::string limit;
        int expected_exit;
        std::vector<std::string> facts;
        const char* executable = SHRINK_PROGRAM_FIXTURE;
    };
    for (const auto& test : std::vector<Case>{
        {"solution timeout", {"rendezvous", "ready", "missing"}, {"rendezvous", "other-ready", "missing"},
         "--solution-timeout=100ms", 3, {"timeout", "peer stop"}},
        {"reference timeout", {"text", "1"}, {"rendezvous", "other-ready", "missing"},
         "--reference-timeout=100ms", 3, {"unsuccessful reference execution (inconclusive)"}},
        {"overall expiry", {"rendezvous", "ready", "missing"}, {"rendezvous", "other-ready", "missing"},
         "--time-limit=100ms", 4, {"overall time limit"}},
        {"launch failure", {}, {"text", "2"}, "", 1, {"Error:"}, "./missing-program"}}) {
        INFO(test.name);
        auto arguments = fixture_arguments(workspace, test.solution, test.reference);
        arguments[3] = test.executable;
        if (!test.limit.empty()) arguments.push_back(test.limit);
        std::ostringstream output, errors;
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == test.expected_exit);
        REQUIRE(output.str().empty());
        REQUIRE(workspace.read("result.txt") == "previous result\n");
        for (const auto& fact : test.facts) REQUIRE(errors.str().find(fact) != std::string::npos);
    }
}
TEST_CASE("CLI caps stderr preview after escaping control bytes", "[cli-baseline]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"fail-stderr", "70000", "1"}, {"text", "2"});
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 3);
    const auto text = errors.str();
    const auto begin = text.find("  stderr: \"");
    REQUIRE(begin != std::string::npos);
    const auto content = begin + std::string("  stderr: \"").size();
    const auto end = text.find('"', content);
    REQUIRE(end != std::string::npos);
    REQUIRE(end - content <= 4096);
    REQUIRE(text.find("\\x01") != std::string::npos);
    REQUIRE(text.find("[presentation truncated]") != std::string::npos);
    REQUIRE(text.find("retention truncated at 64 KiB") != std::string::npos);
}
TEST_CASE("CLI budgeted removal saves smaller evidence and resume verifies with distinct destination", "[cli-removal]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"record-input", "solution-input", "1"}, {"record-input", "reference-input", "2"});
    arguments[16] = "--max-evaluations=1";
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 4);
    REQUIRE(output.str().empty());
    REQUIRE(workspace.read("result.txt") == "2\n-2 7\n");
    REQUIRE(workspace.read("solution-input") == "3\n7 -2 7\n2\n-2 7\n");
    REQUIRE(errors.str().find("Reduction round unfinished") != std::string::npos);
    REQUIRE(errors.str().find("candidate evaluations: 1; acceptances: 1") != std::string::npos);
    REQUIRE(errors.str().find("Final: both programs succeeded") != std::string::npos);
    arguments[1] = "result.txt"; arguments[15] = "resumed.txt";
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 4);
    REQUIRE(workspace.read("result.txt") == "2\n-2 7\n");
    REQUIRE(workspace.read("resumed.txt") == "1\n7\n");
    REQUIRE(workspace.read("solution-input") == "3\n7 -2 7\n2\n-2 7\n2\n-2 7\n1\n7\n");
}
TEST_CASE("CLI throttles progress and quiet retains evidence summary and saved result", "[cli-removal]") {
    Workspace workspace;
    for (bool quiet : {false, true}) {
        auto arguments = fixture_arguments(workspace, {"slow-text", "600", "1"}, {"text", "2"});
        arguments[16] = "--max-evaluations=2";
        if (quiet) arguments.push_back("--quiet");
        std::ostringstream output, errors;
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 4);
        REQUIRE(output.str().empty());
        REQUIRE(workspace.read("result.txt") == "1\n7\n");
        auto progress = errors.str().find("Progress:");
        REQUIRE((progress == std::string::npos) == quiet);
        if (!quiet) {
            std::istringstream lines(errors.str());
            std::string line;
            std::uint64_t previous = 0;
            while (std::getline(lines, line)) {
                if (!line.starts_with("Progress:")) continue;
                REQUIRE(line.find("evaluations ") != std::string::npos);
                REQUIRE(line.find("; acceptances ") != std::string::npos);
                REQUIRE(line.find("; best length ") != std::string::npos);
                const auto marker = line.find("; elapsed ");
                REQUIRE(marker != std::string::npos);
                const auto elapsed = std::stoull(line.substr(marker + 10));
                REQUIRE(elapsed - previous >= 1000);
                previous = elapsed;
            }
        }
        REQUIRE(errors.str().find("Final: both programs succeeded") != std::string::npos);
        REQUIRE(errors.str().find("candidate evaluations: 2; acceptances: 2") != std::string::npos);
    }
}
TEST_CASE("CLI failed final replacement recovers full reduced best and retains saved baseline", "[cli-removal]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"text", "1"}, {"text", "2"});
    arguments[16] = "--max-evaluations=1";
    shrink::cli::InvocationServices services;
    services.save = [](const fs::path& path, const shrink::SaveRequest& save) {
        shrink::cli::SaveFileOperations operations;
        if (save.kind == shrink::SaveKind::final)
            operations.rename = [](const char*, const char*) { errno = EACCES; return -1; };
        return shrink::cli::save_atomic(path, save, operations);
    };
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors, services) == 1);
    REQUIRE(output.str() == "2\n-2 7\n");
    REQUIRE(workspace.read("result.txt") == "3\n7 -2 7\n");
    REQUIRE(errors.str().find("Saved revision 1:") != std::string::npos);
    REQUIRE(errors.str().find("Final save: failed") != std::string::npos);
    REQUIRE(errors.str().find("Observed: evaluation limit") != std::string::npos);
}
TEST_CASE("CLI unrestricted search completes with a precise local guarantee and reused final evidence", "[cli-values]") {
    Workspace workspace;
    auto arguments = fixture_arguments(workspace, {"record-input", "solution-input", "1"}, {"record-input", "reference-input", "2"});
    arguments.erase(arguments.begin() + 16); // No evaluation budget.
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 0);
    REQUIRE(workspace.read("result.txt") == "0\n\n");
    REQUIRE(output.str().empty());
    REQUIRE(errors.str().find("Complete final round") != std::string::npos);
    REQUIRE(errors.str().find("Irreducible under supported local checks") != std::string::npos);
    REQUIRE(errors.str().find("every single-element removal") != std::string::npos);
    REQUIRE(errors.str().find("one occurrence of a repeated value") != std::string::npos);
    REQUIRE(errors.str().find("first differing token: 1") != std::string::npos);
    REQUIRE(errors.str().find("irreducibility was not established") == std::string::npos);
    REQUIRE(workspace.read("solution-input") == "3\n7 -2 7\n2\n-2 7\n1\n7\n0\n\n");
}
TEST_CASE("CLI simplifies duplicate failures and retains completed guarantee when final saving fails", "[cli-values]") {
    Workspace workspace;
    workspace.write("original input.txt", "3 42 7 42");
    auto arguments = fixture_arguments(workspace, {"duplicate-bug"}, {"text", "ok"});
    arguments.erase(arguments.begin() + 16);
    for (bool fail_final : {false, true}) {
        shrink::cli::InvocationServices services;
        services.save = [fail_final](const fs::path& path, const shrink::SaveRequest& save) {
            shrink::cli::SaveFileOperations operations;
            if (fail_final && save.kind == shrink::SaveKind::final)
                operations.rename = [](const char*, const char*) { errno = EACCES; return -1; };
            return shrink::cli::save_atomic(path, save, operations);
        };
        std::ostringstream output, errors;
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors, services) == (fail_final ? 1 : 0));
        REQUIRE(output.str() == (fail_final ? "2\n0 0\n" : ""));
        REQUIRE(errors.str().find("Complete final round") != std::string::npos);
        REQUIRE(errors.str().find("Irreducible under supported local checks") != std::string::npos);
        REQUIRE(errors.str().find("Final: both programs succeeded") != std::string::npos);
        REQUIRE(workspace.read("result.txt") == (fail_final ? "3\n42 7 42\n" : "2\n0 0\n"));
    }
}
TEST_CASE("CLI completed cache modes preserve real-program result and counts without a cache flag", "[cli-cache]") {
    Workspace workspace;
    workspace.write("original input.txt", "3 42 7 42");
    auto arguments = fixture_arguments(workspace, {"duplicate-bug"}, {"text", "ok"});
    arguments.erase(arguments.begin() + 16);
    std::uint64_t uncached_evaluations = 0;
    for (const auto& cache : std::vector<shrink::detail::CacheControls>{
        {.enabled = false}, {}, {.forced_fingerprint = 0}, {.max_entries = 1, .forced_fingerprint = 0}, {.max_text_bytes = 4}}) {
        shrink::cli::InvocationServices services; services.cache = cache;
        std::ostringstream output, errors;
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors, services) == 0);
        REQUIRE(output.str().empty());
        REQUIRE(workspace.read("result.txt") == "2\n0 0\n");
        REQUIRE(errors.str().find("Complete final round; distinct inconclusive candidates: 0") != std::string::npos);
        REQUIRE(errors.str().find("acceptances: 2") != std::string::npos);
        REQUIRE(errors.str().find("Final: both programs succeeded") != std::string::npos);
        const std::string label = "candidate evaluations: ";
        auto marker = errors.str().find(label); REQUIRE(marker != std::string::npos);
        const auto evaluations = std::stoull(errors.str().substr(marker + label.size()));
        if (!cache.enabled) uncached_evaluations = evaluations;
        else REQUIRE(evaluations <= uncached_evaluations);
    }
    arguments.push_back("--cache=false");
    std::ostringstream output, errors;
    REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors) == 2);
}
TEST_CASE("CLI cached inconclusive checks retain exact uncertainty counts with fewer executions", "[cli-cache]") {
    Workspace workspace;
    workspace.write("original input.txt", "2 5 5");
    auto arguments = fixture_arguments(workspace, {"text", "1"}, {"original-pair-reference"});
    arguments.erase(arguments.begin() + 16);
    for (const auto& cache : std::vector<shrink::detail::CacheControls>{
        {.enabled = false}, {}, {.forced_fingerprint = 0}, {.max_entries = 1, .forced_fingerprint = 0}}) {
        shrink::cli::InvocationServices services; services.cache = cache;
        std::ostringstream output, errors;
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors, services) == 0);
        REQUIRE(workspace.read("result.txt") == "2\n5 5\n");
        REQUIRE(output.str().empty());
        REQUIRE(errors.str().find("Complete final round; distinct inconclusive candidates: 7") != std::string::npos);
        REQUIRE(errors.str().find(cache.enabled ? "candidate evaluations: 7; acceptances: 0" : "candidate evaluations: 8; acceptances: 0") != std::string::npos);
        REQUIRE(errors.str().find(cache.enabled ? "inconclusive executions: 7" : "inconclusive executions: 8") != std::string::npos);
    }
}
namespace {
// Run real programs, then simulate sleep reaching the exact overall deadline
// only after both readiness records are complete. Cleanup uses real elapsed
// time after the jump. No assumed startup time or short sleep determines order.
class ReadyDeadlineClock final : public shrink::detail::ExecutionBackend {
public:
    std::unique_ptr<shrink::detail::ExecutionBackend> native = shrink::detail::make_mac_execution_backend();
    std::optional<std::uint64_t> jumped_at;
    std::vector<pid_t> children;
    std::vector<shrink::ProgramRole> stops;
    std::uint64_t now_ms() override {
        const auto now = native->now_ms();
        if (!jumped_at) {
            pid_t solution = 0, reference = 0;
            std::ifstream left("solution-ready"), right("reference-ready");
            if (!(left >> solution) || !(right >> reference) || solution <= 0 || reference <= 0) return 0;
            jumped_at = now;
        }
        return 1000 + now - *jumped_at;
    }
    std::optional<shrink::RunnerError> launch(shrink::ProgramRole role, const shrink::ProgramSpec& program, std::string_view input) override {
        auto error = native->launch(role, program, input);
        if (const auto pid = native->process_id(role)) children.push_back(*pid);
        return error;
    }
    std::vector<shrink::detail::ExecutionEvent> service(shrink::CancellationToken cancellation, int wait_ms) override {
        return native->service(cancellation, wait_ms);
    }
    std::optional<shrink::RunnerError> stop(shrink::ProgramRole role) override { stops.push_back(role); return native->stop(role); }
    bool pending(shrink::ProgramRole role) const override { return native->pending(role); }
    std::optional<int> process_id(shrink::ProgramRole role) const override { return native->process_id(role); }
    shrink::PendingOwnership ownership(shrink::ProgramRole role) const override { return native->ownership(role); }
};
}
TEST_CASE("CLI overall expiry terminates both real programs and retains only verified recovery", "[cli-time-limit]") {
    struct Case { bool verified; bool fail_final; };
    for (const auto& test : {Case{false, false}, Case{true, false}, Case{true, true}}) {
        const auto verified = test.verified;
        Workspace workspace;
        const std::string threshold = verified ? "3" : "4";
        auto arguments = fixture_arguments(workspace, {"wait-below-length", "solution-ready", "1", threshold},
                                                      {"wait-below-length", "reference-ready", "2", threshold});
        arguments.erase(arguments.begin() + 16);
        arguments.insert(arguments.end(), {"--time-limit=1s", "--solution-timeout=10s", "--reference-timeout=10s"});
        auto backend = std::make_unique<ReadyDeadlineClock>(); auto* observed = backend.get();
        shrink::Runner runner(std::move(backend));
        shrink::cli::InvocationServices services; services.runner = &runner;
        services.save = [&](const fs::path& path, const shrink::SaveRequest& save) -> shrink::SaveResult {
            if (test.fail_final && save.kind == shrink::SaveKind::final)
                return shrink::SaveError{shrink::SaveErrorCode::replace, "final save after expiry", path, std::make_error_code(std::errc::permission_denied)};
            return shrink::cli::save_atomic(path, save);
        };
        std::ostringstream output, errors;
        REQUIRE(shrink::cli::run(Workspace::views(arguments), output, errors, services) == (test.fail_final ? 1 : 4));
        REQUIRE(observed->jumped_at);
        REQUIRE(observed->stops == std::vector{shrink::ProgramRole::solution, shrink::ProgramRole::reference});
        REQUIRE_FALSE(runner.has_pending_ownership());
        REQUIRE(observed->children.size() == (verified ? 4 : 2));
        for (auto child : observed->children) {
            int status = 0;
            REQUIRE(waitpid(child, &status, WNOHANG) == -1);
            REQUIRE(errno == ECHILD);
        }
        REQUIRE(output.str() == (test.fail_final ? "3\n7 -2 7\n" : ""));
        REQUIRE(workspace.read("result.txt") == (verified ? "3\n7 -2 7\n" : "previous result\n"));
        REQUIRE(errors.str().find("Observed: overall time limit") != std::string::npos);
        REQUIRE(errors.str().find("Observed: caller cancellation") == std::string::npos);
        REQUIRE(errors.str().find("irreducibility was not established") != std::string::npos);
        REQUIRE(errors.str().find(verified ? "Final: both programs succeeded" : "No verified result was produced") != std::string::npos);
    }
}
