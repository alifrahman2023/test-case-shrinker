# Try shrink: five wrong-answer examples

Each folder contains a faulty `solution` binary, a trusted `reference` binary,
their standalone C++ sources, an `original.txt` input, problem/constraint notes,
and a `run.sh` script with the appropriate shrink command.

| Folder | Problem | Deliberate mistake |
| --- | --- | --- |
| [duplicates](duplicates/README.md) | Count distinct values | Assumes equal values are adjacent. |
| [width](width/README.md) | Multiply two values | Uses a 32-bit product instead of 64 bits. |
| [maximum](maximum/README.md) | Find the largest value | Initializes the maximum to zero. |
| [sum-last](sum-last/README.md) | Sum all elements | Omits the last element. |
| [lower-median](lower-median/README.md) | Find the lower median | Chooses the upper median for even lengths. |

## Run an example

Install or build `shrink` using the [installation instructions](../README.md#installation),
then run from the repository root:

```sh
./examples/duplicates/solution < examples/duplicates/original.txt
./examples/duplicates/reference < examples/duplicates/original.txt
./examples/duplicates/run.sh
cat examples/duplicates/reduced.txt
```

Repeat with any folder in the table. Each script works from any working directory
and saves `reduced.txt` beside its input by default. Pass a different output path
as its first argument. Existing output is replaced after successful verification.
Scripts use `SHRINK_BIN` when set, otherwise an installed `shrink`, then a local
`build/release/shrink` or `build/debug/shrink` executable.
The tutorial commands allow ten seconds per program, including executable startup
on macOS. They leave overall time and evaluation budgets unrestricted.

```sh
SHRINK_BIN="$PWD/build/debug/shrink" ./examples/maximum/run.sh /tmp/maximum.txt
```

The committed solution/reference binaries are example fixtures for **macOS 15+
on Apple Silicon (ARM64)**. They are separate from the shrink command itself.
Build them again after editing their sources, or to use your local compiler:

```sh
./examples/build.sh
```

This requires Apple's Command Line Tools and compiles all ten programs with
Apple Clang, C++20, optimization, and a macOS 15 deployment target. Generated
reduced inputs stay local. Shrink still accepts already compiled programs and
does not compile them itself.

## Detailed demonstrations

Run these commands from the repository root on native macOS with Apple Clang
and CMake 3.24 or newer. The supported baseline is macOS 15 ARM64. Both examples
read one integer array from stdin and print one integer answer to stdout.

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel 3
mkdir -p build/demo-results
```

CMake compiles the demonstration programs for the Mac where they run.
The initial test configuration fetches pinned Catch2 from GitHub. Shrink takes
already compiled executable paths; it does not compile user programs. Only
`shrink` is installed; CMake's compiled programs stay in the build directory.

## Duplicate handling: count distinct values

The task is to count distinct values in an array. All declared input constraints:
length **0 through 10**, values **-100 through 100**, arbitrary order, duplicates
allowed. Do not pass `--sorted` or `--unique`.

The [trusted reference](duplicates/reference.cpp) inserts every value into a set,
so its answer counts each value once regardless of position. The
[faulty solution](duplicates/solution.cpp) counts runs of equal neighbors,
incorrectly assuming equal values are adjacent. The [original input](duplicates/original.txt)
is `[42, -7, 42, 9]`.

```sh
build/debug/demo_duplicates_solution < examples/duplicates/original.txt
# 4
build/debug/demo_duplicates_reference < examples/duplicates/original.txt
# 3
build/debug/shrink \
  --input examples/duplicates/original.txt \
  --solution build/debug/demo_duplicates_solution \
  --reference build/debug/demo_duplicates_reference \
  --min-length 0 --max-length 10 --min-value=-100 --max-value 100 \
  --output build/demo-results/duplicates.txt --quiet
cat build/demo-results/duplicates.txt
# 3
# 0 1 0
build/debug/demo_duplicates_solution < build/demo-results/duplicates.txt
# 3
build/debug/demo_duplicates_reference < build/demo-results/duplicates.txt
# 2
```

The current repeatable strategy first removes the distracting `9`, then simplifies
equal values together. Changing only one of the two `42`s would lose their equality.
On the final `[0, 1, 0]`, removing any element loses the disagreement. Changing the
`1` to `0` makes both answers `1`; changing either zero toward a simpler value is
impossible. Thus the supported local checks cannot simplify it further.

## Arithmetic width: multiply two values

The task is to print the mathematical product of two nonnegative integers.
All declared input constraints: length **exactly 2**, values **0 through 100000**,
arbitrary order, duplicates allowed. Do not pass `--sorted` or `--unique`.

The [trusted reference](width/reference.cpp) multiplies in `std::uint64_t`.
The largest permitted product, 10000000000, fits in that type. The
[faulty solution](width/solution.cpp) multiplies in `std::uint32_t`: on the supported
Mac target this unsigned arithmetic wraps modulo 4294967296. This deliberately
reproduces insufficient arithmetic width without undefined signed overflow.
The [original input](width/original.txt) is `[100000, 100000]`.

```sh
build/debug/demo_width_solution < examples/width/original.txt
# 1410065408
build/debug/demo_width_reference < examples/width/original.txt
# 10000000000
build/debug/shrink \
  --input examples/width/original.txt \
  --solution build/debug/demo_width_solution \
  --reference build/debug/demo_width_reference \
  --min-length 2 --max-length 2 --min-value 0 --max-value 100000 \
  --output build/demo-results/width.txt --quiet
cat build/demo-results/width.txt
# 2
# 65536 65536
build/debug/demo_width_solution < build/demo-results/width.txt
# 0
build/debug/demo_width_reference < build/demo-results/width.txt
# 4294967296
```

Equal-value simplification moves both elements together to `65536` with the
current strategy. Removing an element violates the length constraint. Replacing
either element with zero loses the wrong answer, as does moving one or both
elements one unit toward zero: 65535 × 65536 = 4294901760 fits in 32 bits.
There are no negative counterparts to check. This establishes local irreducibility
for this result; the acceptance test checks these mathematical conditions without
requiring this exact final pair from future strategies.

## Read the evidence and understand the guarantee

Both reduction commands finish with exit status **0**. Ordinary stdout is empty;
the normalized reduced input is in the destination file. Stderr reports baseline
and final evidence: both programs succeeded, the first differing token is **1**,
and the solution/reference token previews match the answers above. Child stdout
is captured, not forwarded. `--quiet` suppresses periodic progress only.

The summary reports a complete final round, **zero distinct inconclusive
candidates**, local irreducibility, and a successful final save. The tests run the
saved input through both programs independently; shrink itself retains its
accepted evidence without an extra final execution.

An irreducible input is locally irreducible under the supported single-element
removals, all-occurrences value changes, and individual changes of repeated
values to the simplest allowed value, one unit toward zero, and representable
positive counterparts of negative values. Each applicable strictly smaller check
was invalid, lost the wrong answer, or was inconclusive. Inconclusive checks mean
the solution succeeded but the reference did not, leaving uncertainty about
further reductions. These small examples have none. There is **no global smallest
input or same-underlying-bug guarantee**. Budget, cancellation, or operational
stops may leave further reduction possible and do not establish irreducibility.

## Recover with a fresh invocation

Use a saved input as the next original input, repeat all required program and
constraint options, and choose a **different destination**:

```sh
build/debug/shrink \
  --input build/demo-results/duplicates.txt \
  --solution build/debug/demo_duplicates_solution \
  --reference build/debug/demo_duplicates_reference \
  --min-length 0 --max-length 10 --min-value=-100 --max-value 100 \
  --output build/demo-results/duplicates-again.txt --max-evaluations 0 --quiet
# Exit status 4: verified and saved the baseline; no reduction round began.
```

Omit `--max-evaluations 0` to search again. This is fresh verification and search;
no search or cache state is restored. Input/output aliases are rejected, including
the default `reduced.txt` when that is also the input. A time/evaluation limit
returns 4; Ctrl-C returns 130 unless an operational error requires status 1.

After verification, shrink saves the normalized baseline, checkpoints pending
improvements between evaluations at most once per second, and attempts one final
save on stopping. After a hard kill, use the last successfully saved file if one
exists; the interval is an I/O throttle, not a maximum recovery gap. Temporary-file
replacement protects the prior destination on a failed write, without promising
power-loss durability. A final-save failure returns **1** and attempts to emit the
complete normalized best known failing input on stdout for recovery; errors remain
on stderr. To capture that fallback, append `> build/demo-results/recovery.txt` to
an invocation. That file is empty on ordinary success. No verified recovery input
exists before successful baseline verification.

## Prerequisites and automated checks

You are responsible for reference correctness and for declaring constraints
sufficient to express the problem requirements. Built-in bounds, sortedness and
uniqueness cannot express every problem constraint. Both examples are deterministic,
use only stdin and stdout, create no descendants, and need no external state.
Keep the programs and relevant environment stable during a run, with execution
times comfortably below the per-program timeouts. Repeatability excludes time
limits, interruption, near-timeout executions, and changes of shrink version.

The first release supports wrong-answer reduction on one integer array, with both
solution and reference required. Crash reduction and Linux support are deferred.
Supplied programs **must create no descendants**; shrink neither detects nor
prevents them. Programs execute with your permissions and inherited environment;
there is **no filesystem or network sandbox**. Use programs you trust to run.

```sh
ctest --test-dir build/debug -R '^demo_' --output-on-failure --no-tests=error
```

Each CTest check executes the real CLI, checks known original answers, independently
parses the saved input and validates its declared constraints, checks the result's
answers and local conditions, and verifies fresh baseline recovery into a new file.
Saved files and evidence remain under `build/debug/demo-duplicates-check` and
`build/debug/demo-width-check`. The full Debug CTest suite includes both checks.
CI also repeats them with the installed Release command outside its build directory.
