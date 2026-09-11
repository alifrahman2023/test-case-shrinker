#include <shrink/version.hpp>

#include <CLI/CLI.hpp>

#include <csignal>
#include <iostream>
#include <string>

namespace {

// Exit codes, per plan §10. Anything not listed here is not a documented outcome.
constexpr int kExitShrunk = 0;          // shrinking succeeded
constexpr int kExitNotInteresting = 1;  // original input does not reproduce the failure
constexpr int kExitSpecError = 2;       // spec or input parse error
constexpr int kExitSpawnError = 3;      // binary missing or spawn failure
constexpr int kExitInternal = 4;        // internal invariant violated

// Temporary, for the M0 skeleton only: the pipeline behind these flags does not exist yet.
// Removed once the driver lands (M4).
constexpr int kExitNotImplemented = 70;  // EX_SOFTWARE

}  // namespace

int main(int argc, char** argv) {
  // Writing to a child that already exited must not kill the shrinker (plan §9.1).
  // Installed before anything can spawn a process, which means here.
  std::signal(SIGPIPE, SIG_IGN);

  CLI::App app{"shrink — format-aware test-case reducer", "shrink"};
  app.set_version_flag("-V,--version", std::string{shrink::version()});

  std::string input_file;
  app.add_option("input-file", input_file, "failing input to reduce")->required();

  CLI11_PARSE(app, argc, argv);

  std::cerr << shrink::version_banner()
            << ": skeleton only — reduction is not implemented yet.\n"
               "Received input file: "
            << input_file << '\n';
  return kExitNotImplemented;
}

// Silence unused-constant warnings until the driver consumes these (M4).
static_assert(kExitShrunk == 0 && kExitNotInteresting == 1 && kExitSpecError == 2 &&
              kExitSpawnError == 3 && kExitInternal == 4);
