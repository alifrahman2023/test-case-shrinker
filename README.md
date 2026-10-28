# shrink

A native macOS CLI for reducing wrong-answer integer-array inputs.
The current implementation provides `--help`, `--version`, and request/input/file
validation. The internal core also evaluates wrong-answer candidates with concurrent, bounded programs; CLI execution and reduction remain subsequent tickets under the [approved spec](https://github.com/alifrahman2023/test-case-shrinker/issues/10).

The supported baseline is macOS 15 on Apple Silicon (ARM64), using Apple Clang
and C++20. Newer macOS versions are not rejected. Development requires Apple's
Command Line Tools (`xcode-select --install`) and CMake 3.24 or newer.
CMake prints the actual compiler and CMake versions during configuration.

Build and test natively:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel 3
ctest --test-dir build/debug --output-on-failure --no-tests=error
```

Test builds automatically fetch the pinned Catch2 v3.8.1 release from GitHub,
so the initial configuration needs network access. To run one test group:

```sh
build/debug/shrink_core_tests '[input]'
build/debug/shrink_cli_tests '[cli]'
ctest --test-dir build/debug -R cli_information --output-on-failure
```

Build and install the optimized command without Catch2:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/release --parallel 3
cmake --install build/release --prefix "$HOME/.local"
```

Put `$HOME/.local/bin` on `PATH` to invoke `shrink` normally. Verify the installed
command from outside the build directory, without any reduction configuration:

```sh
cd /tmp
"$HOME/.local/bin/shrink" --help
"$HOME/.local/bin/shrink" --version
```

Only `bin/shrink` is installed. The internal static `shrink_core` library is
shared by the CLI and direct Catch2 tests; it is not a public SDK and has no
stable API or ABI. Ordinary `.hpp`/`.cpp` boundaries under `src/core` separate
input format, coordinator, evaluator, and runner responsibilities. The input
format and coordinator prepare typed requests; the evaluator classifies exact
output-token disagreement while the runner owns concurrent Mac execution.
Flags and terminal output belong in `src/cli`.

Every pull request and update to `main` runs the full Debug CTest suite and an
installed CLI-only Release check on the named `macos-15` ARM64 runner. New tests
must be registered with CTest so this same job covers subsequent slices.
The `macOS 15 ARM64 Debug and installed Release` check must pass before merging
or releasing; configure it as a required status check in repository rules.
Release packaging and Homebrew verification are covered by a later ticket.

Request validation requires `--input`, `--solution`, `--reference`, and all four
inclusive bounds: `--min-length`, `--max-length`, `--min-value`, `--max-value`.
Run `shrink --help` for optional ordering, uniqueness, program arguments, limits,
and output settings. Value options support both separate and equals forms;
program arguments beginning with `-` require `--solution-arg=VALUE` or
`--reference-arg=VALUE`. Paths and arguments receive no shell expansion.

Input contains one nonnegative length followed by exactly that many signed
64-bit integers. Integers use plain decimal spelling with no plus sign, leading
zeros, or negative zero. Lengths and length bounds range from zero to `INT64_MAX`.
Values and value bounds cover the entire signed 64-bit range. Normalization
preserves values and order, using a length line, a single-space-separated value
line, and a final newline (`0\n\n` for an empty array).

Malformed input, inconsistent constraints, invalid options, and unsuitable files
return status 2 before executing either program or changing any result file.
Input-read failures return status 1. Ordinary stdout stays empty, and diagnostics
go to stderr. The core exposes typed validation errors; the CLI file adapter
retains operation, path, and system-error details for file I/O failures.
A valid invocation currently returns status 1 with an explicit execution-unavailable
message. It creates no verified result until baseline evaluation is implemented.

Input and output must be regular files (a new output is allowed), and the output
parent must exist. Input and executable symlinks are permitted; output symlinks
are rejected. Output may not alias the original input or either executable,
including through hard links or symlinks in protected paths or parent directories.

Internal candidate evaluation accepts already normalized bytes, two explicit
`ProgramSpec` paths with ordered arguments, positive per-program timeouts, and a
caller-owned `CancellationToken`. Programs inherit the working directory,
environment, and intended signal state. They must create no descendants. The
runner installs no application-wide signal handlers and intercepts cancellation
and deadline waits at intervals no longer than 5 ms when scheduled.

Each program retains at most 8,388,608 unchanged stdout bytes (one extra byte
fails execution) and the first 65,536 stderr bytes (extra bytes are drained and
marked). `Evaluation` owns its output and records disagreement using byte offsets
and a one-based token position; end-of-output is explicit. It has no ordinary
outcome on cancellation or operational error. Exit/signal facts remain separate
from timeout, overflow, cancellation, and peer-stop reasons. A completed wrong
answer requires zero exits, complete output, and successful cleanup for both.

`Runner` is caller-owned and confined to one synchronous evaluation at a time.
Hosts must not independently reap its children or install incompatible SIGCHLD
handling. Required stops use immediate SIGKILL and a shared maximum one-second
cleanup wait. If cleanup remains unresolved, the result reports a typed error
and known process facts, `has_pending_ownership()` remains true, and further
evaluations are refused. The runner retains the actual process/descriptor
ownership; diagnostic process identifiers never authorize caller signaling or
reaping. Destroying a runner with pending ownership terminates as a caller
contract violation, without an indefinite destructor wait. Explicit bounded
recovery and complete lifecycle integration are the follow-on ownership slice;
the CLI does not yet invoke this evaluator.

Run the native execution and controlled clock/event groups with:

```sh
build/debug/shrink_core_tests '[runner],[execution-events]'
```
