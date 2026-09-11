#include <shrink/ir.hpp>
#include <shrink/runner.hpp>
#include <shrink/spec.hpp>
#include <shrink/version.hpp>

#include <CLI/CLI.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

// Exit codes, per plan §10. Anything not listed here is not a documented outcome.
constexpr int kExitShrunk = 0;          // shrinking succeeded
constexpr int kExitNotInteresting = 1;  // original input does not reproduce the failure
constexpr int kExitSpecError = 2;       // spec or input parse error
constexpr int kExitSpawnError = 3;      // binary missing or spawn failure
constexpr int kExitInternal = 4;        // internal invariant violated

// Temporary, until the driver lands (M4): the reduction pipeline does not exist yet.
constexpr int kExitNotImplemented = 70;  // EX_SOFTWARE

std::string preset_list() {
  std::string out;
  for (const auto& name : shrink::preset_names()) {
    if (!out.empty()) out += " | ";
    out += name;
  }
  return out;
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in{path, std::ios::binary};
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  // Writing to a child that already exited must not kill the shrinker (plan §9.1).
  // Installed before anything can spawn a process, which means here.
  shrink::install_sigpipe_guard();

  CLI::App app{"shrink — format-aware test-case reducer", "shrink"};
  app.set_version_flag("-V,--version", std::string{shrink::version()});

  std::string input_file;
  std::string format;
  std::string preset;
  bool dry_run = false;

  app.add_option("input-file", input_file, "failing input to reduce")->required();
  auto* format_opt = app.add_option("--format", format, "format DSL string, e.g. \"n; a[n]\"");
  app.add_option("--preset", preset, "built-in format: " + preset_list())->excludes(format_opt);
  app.add_flag("--dry-run", dry_run, "parse the spec and input, print the tree, run nothing");

  CLI11_PARSE(app, argc, argv);

  if (format.empty() && !preset.empty()) {
    const auto dsl = shrink::preset_spec(preset);
    if (dsl.empty()) {
      std::cerr << "shrink: unknown preset '" << preset << "'. Known presets: " << preset_list()
                << '\n';
      return kExitSpecError;
    }
    format = std::string{dsl};
  }
  if (format.empty()) {
    std::cerr << "shrink: a format is required. Pass --format \"n; a[n]\" or --preset <"
              << preset_list() << ">\n";
    return kExitSpecError;
  }

  const auto spec = shrink::parse_spec(format);
  if (!spec.ok()) {
    std::cerr << "shrink: " << spec.error << '\n';
    return kExitSpecError;
  }

  std::string text;
  if (!read_file(input_file, text)) {
    std::cerr << "shrink: cannot read '" << input_file << "'\n";
    return kExitSpawnError;
  }

  const auto tree = shrink::parse_input(spec.value, text);
  if (!tree.ok()) {
    std::cerr << "shrink: " << input_file << ": " << tree.error << '\n';
    return kExitSpecError;
  }

  if (dry_run) {
    std::cerr << "format: " << format << "\n\n";
    std::cerr << shrink::dump(spec.value, tree.value) << '\n';
    std::cout << shrink::serialize(spec.value, tree.value);
    return kExitShrunk;
  }

  std::cerr << shrink::version_banner()
            << ": the input parses, but reduction is not implemented yet. Try --dry-run.\n";
  return kExitNotImplemented;
}

// Silence unused-constant warnings until the driver consumes these (M4).
static_assert(kExitShrunk == 0 && kExitNotInteresting == 1 && kExitSpecError == 2 &&
              kExitSpawnError == 3 && kExitInternal == 4);
