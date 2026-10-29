# Install shrink from source

The supported baseline is **macOS 15 on Apple Silicon (ARM64)**. Newer macOS
versions are checked separately before release. Use Apple's Command Line Tools
(`xcode-select --install`) and CMake 3.24 or newer. Both installation paths build
an optimized executable locally; there are no bottles or downloadable binaries.
Only `bin/shrink` is installed, without a library or SDK. Linux, Intel Macs,
Docker, crash reduction, and supplied programs with descendants are deferred.

## Homebrew

The personal tap lives in this repository's `Formula/` directory. The formula
targets `v0.1.0`. **That tag has not been published yet**; these commands become
usable after the author approves and verifies that release.

```sh
brew tap alifrahman2023/shrink https://github.com/alifrahman2023/test-case-shrinker
brew install --build-from-source alifrahman2023/shrink/shrink
brew test alifrahman2023/shrink/shrink
shrink --help
shrink --version
```

Homebrew supplies the build-only CMake dependency. Test builds additionally fetch
pinned Catch2 v3.8.1; installing the command does not fetch or install Catch2.

## Manual CMake fallback

Install CMake (for example `brew install cmake`) and the Command Line Tools first.
Use the approved tagged source, rather than the moving development branch:

```sh
curl --fail --location \
  https://github.com/alifrahman2023/test-case-shrinker/archive/refs/tags/v0.1.0.tar.gz \
  --output shrink-v0.1.0.tar.gz
tar -xzf shrink-v0.1.0.tar.gz
cd test-case-shrinker-0.1.0
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/release --parallel 3
cmake --install build/release --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
cd /tmp
shrink --help
shrink --version
```

Add the `PATH` setting to your shell configuration for future terminals. A
user-local prefix needs no administrator privileges. The release verification
procedure is in [RELEASING.md](RELEASING.md); a passing development checkout is
not evidence that a particular published tag was verified.

## Supplied programs and guarantees

Compile both your solution and trusted reference for the Mac where they run.
Shrink accepts executable paths and arguments; it does not compile user programs.
Programs must be deterministic, create no descendants, and remain unchanged with
their relevant external state during the run. Threads are permitted under that
determinism requirement. You supply a correct reference and sufficient declared
input constraints. Programs run with your permissions and inherited environment;
execution is not sandboxed.

See the [technical reference](REFERENCE.md) for the exact local irreducibility guarantee,
repeatability exclusions, sleep-inclusive limits, bounded cleanup, checkpoint
recovery, and internal callback/runner lifetime rules. See the
[demonstrations](examples/README.md) for complete real-program invocations.
