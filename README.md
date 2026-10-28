# shrink

A native macOS CLI for reducing wrong-answer integer-array inputs.
The current implementation provides `--help` and `--version`; reduction is being
implemented in subsequent tickets under the [approved spec](https://github.com/alifrahman2023/test-case-shrinker/issues/10).

The supported baseline is macOS 15 on Apple Silicon (ARM64), using Apple Clang
and C++20. Newer macOS versions are not rejected. Development requires Apple's
Command Line Tools (`xcode-select --install`) and CMake 3.24 or newer.
CMake prints the actual compiler and CMake versions during configuration.

Build and test natively:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure --no-tests=error
```

Test builds automatically fetch the pinned Catch2 v3.8.1 release from GitHub,
so the initial configuration needs network access. To run one test group:

```sh
ctest --test-dir build/debug -R cli_information --output-on-failure
ctest --test-dir build/debug -R 'The core' --output-on-failure
```

Build and install the optimized command without Catch2:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/release --parallel
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
input format, coordinator, evaluator, and runner responsibilities. These four
modules currently reserve the boundaries for subsequent implementation tickets.
Flags and terminal output belong in `src/cli`.

Every pull request and update to `main` runs the full Debug CTest suite and an
installed CLI-only Release check on the named `macos-15` ARM64 runner. New tests
must be registered with CTest so this same job covers subsequent slices.
The `macOS 15 ARM64 Debug and installed Release` check must pass before merging
or releasing; configure it as a required status check in repository rules.
Release packaging and Homebrew verification are covered by a later ticket.
