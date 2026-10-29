# shrink

Turn a large wrong-answer input into a smaller example you can debug.

`shrink` is a native macOS command-line tool for competitive programming. Give it
your compiled solution, a trusted reference program, and an integer-array input
where their answers disagree. It removes elements and simplifies values while
preserving an observed wrong answer within your declared constraints.

- [Why shrink?](#why-shrink)
- [Installation](#installation)
- [Quick start](#quick-start)
- [Commands](#commands)
- [Usage examples](#usage-examples)
- [Dependencies](#dependencies)
- [License](#license)

## Why shrink?

- **Less input to inspect.** Remove distracting elements and simplify large values.
- **Keep constraints intact.** Declare length/value bounds, sortedness, and uniqueness.
- **See the disagreement.** Baseline and final evidence show the first differing output token.
- **Keep verified progress.** Checkpoints and a final save retain the best known failing input.
- **Control long searches.** Set evaluation/time budgets or stop with Ctrl-C.

The first release handles wrong answers on one integer array. Both programs must
finish successfully for their disagreement to count. Crash reduction and Linux
support are deferred. The supported baseline is **macOS 15 on Apple Silicon
(ARM64)**; CI also checks macOS 26 ARM64.

## Installation

Source builds require Apple's Command Line Tools (`xcode-select --install`) and
CMake 3.24 or newer. Only the `shrink` executable is installed.

### Homebrew

The formula targets `v0.1.0`, which is **pending release**. Once that tag is
approved and published:

```sh
brew tap alifrahman2023/shrink https://github.com/alifrahman2023/test-case-shrinker
brew install --build-from-source alifrahman2023/shrink/shrink
brew test alifrahman2023/shrink/shrink
```

### Manual

To build the current source and install into your home directory:

```sh
git clone https://github.com/alifrahman2023/test-case-shrinker.git
cd test-case-shrinker
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/release --parallel 3
cmake --install build/release --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
shrink --version
```

Add the `PATH` setting to your shell configuration for future terminals.
See [installation details](INSTALL.md) for tagged-source installation.

## Quick start

Suppose `failing.txt` contains:

```text
4
42 -7 42 9
```

With your solution and reference compiled as `./solution` and `./reference`:

```sh
shrink --input failing.txt --solution ./solution --reference ./reference \
  --min-length 0 --max-length 10 --min-value=-100 --max-value 100 \
  --output reduced.txt
cat reduced.txt
```

Use the actual constraints of your problem. Input is a length followed by exactly
that many signed 64-bit integers, separated by whitespace. Use plain decimal
integers without a leading `+`, leading zeros, or negative zero.

Shrink verifies the normalized original input, then searches for smaller inputs
on which both programs succeed and their output tokens differ. Whitespace
between output tokens is ignored; `1` and `1.0` are different tokens.

The result goes to `reduced.txt`; progress, evidence, and the summary go to
stderr. Ordinary stdout stays empty. Status `0` means local irreducibility under
the supported checks, with any inconclusive checks reported. It does not promise
the globally smallest input or the same underlying bug.

Try [five hands-on examples](examples/README.md), each with solution/reference
binaries and C++ sources, an input, problem notes, and a ready-to-run script.
They cover duplicates, arithmetic width, negative maxima, an omitted last element,
and sorted-array medians.

## Commands

| Command | Purpose |
| --- | --- |
| `shrink [options]` | Verify and reduce a wrong-answer input. |
| `shrink --help` | Show available options. |
| `shrink --version` | Print the version. |

### Required options

| Option | Purpose |
| --- | --- |
| `--input PATH` | Original input file. |
| `--solution PATH` | Compiled solution program. |
| `--reference PATH` | Compiled trusted reference program. |
| `--min-length N`, `--max-length N` | Inclusive array-length bounds. |
| `--min-value N`, `--max-value N` | Inclusive element-value bounds. |

### Optional settings

| Option | Purpose / default |
| --- | --- |
| `--output PATH` | Result and recovery file; default `reduced.txt`. |
| `--sorted ascending\|descending` | Require nondecreasing or nonincreasing values. |
| `--unique` | Require distinct values. |
| `--solution-arg=VALUE` | Append a solution argument; repeatable. |
| `--reference-arg=VALUE` | Append a reference argument; repeatable. |
| `--solution-timeout TIME` | Per-solution elapsed limit; default `2s`. |
| `--reference-timeout TIME` | Per-reference elapsed limit; default `10s`. |
| `--time-limit TIME` | Overall elapsed budget; no default limit. |
| `--max-evaluations N` | Candidate evaluation budget; no default limit. |
| `--quiet` | Suppress periodic progress; retain evidence, summary, and errors. |

Durations are positive whole milliseconds or seconds, such as `250ms` or `10s`.
Value options accept `--option value` and `--option=value`. Program arguments
starting with `-` require the equals form. Shrink uses explicit executable paths
and passes arguments without shell interpretation or PATH lookup.

### Exit statuses

| Status | Meaning |
| --- | --- |
| `0` | Completed local checks, or successful help/version. |
| `1` | Operational error, including execution, cleanup, or saving errors. |
| `2` | Invalid options, constraints, input, or configured file destinations. |
| `3` | The baseline did not establish a wrong-answer failure. |
| `4` | Evaluation or overall time budget reached before completion. |
| `130` | Ctrl-C cancellation. |

Operational errors take precedence over cancellation and budgets.

## Usage examples

### Limit a search

```sh
shrink --input failing.txt --solution ./solution --reference ./reference \
  --min-length 0 --max-length 100000 --min-value=-1000000000 \
  --max-value 1000000000 --max-evaluations 500 --time-limit 30s \
  --output reduced.txt --quiet
```

`--max-evaluations 0` verifies and saves only the baseline, returning status `4`.
Baseline verification, invalid candidates, and cache hits do not consume the
candidate evaluation budget. Time limits include computer sleep; cleanup and
final saving can extend beyond the overall budget.

### Preserve ordering and pass program arguments

```sh
shrink --input sorted.txt --solution ./solution --reference ./reference \
  --min-length 1 --max-length 1000 --min-value 0 --max-value 100000 \
  --sorted ascending --unique \
  --solution-arg=--algorithm --solution-arg=fast --output reduced.txt
```

### Resume from a saved input

```sh
shrink --input reduced.txt --solution ./solution --reference ./reference \
  --min-length 0 --max-length 10 --min-value=-100 --max-value 100 \
  --output reduced-again.txt
```

Repeat the original program and constraint options, using a different destination.
Resume performs fresh verification; it restores no search or cache state.
Input/output aliases are rejected. Existing output is replaced only after
successful baseline verification.

After Ctrl-C, shrink attempts to save verified progress. After a hard kill,
recovery uses the last successful checkpoint, if one exists. Saves use temporary
files and rename without fsync; power-loss durability is not promised. If the
final save fails, shrink returns `1` and attempts to write the complete best known
failing input to stdout for recovery.

## Dependencies

| Dependency | Needed for |
| --- | --- |
| macOS on Apple Silicon | Running shrink; supported baseline macOS 15 ARM64. |
| Apple Command Line Tools / Apple Clang | Building shrink and the C++ demonstrations. |
| CMake 3.24+ | Building and installing; Homebrew supplies it as a build dependency. |
| Catch2 v3.8.1 | Tests only; fetched automatically when `BUILD_TESTING=ON`. |

Supply compiled, deterministic solution and reference programs that create no
descendant processes. Keep them and relevant external state stable during a run.
You are responsible for reference correctness and sufficient declared constraints.
Programs run with your permissions and inherited environment, without sandboxing.

For development commands, exact reduction/repeatability guarantees, execution
limits, and core lifetime contracts, see the [technical reference](REFERENCE.md).
Release checks and runner/dependency maintenance are in [RELEASING.md](RELEASING.md).

## License

A license has not yet been specified for this repository.
