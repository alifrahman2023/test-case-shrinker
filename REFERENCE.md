# Behavior and core reference

Detailed guarantees, execution contracts, and development checks for shrink.
For installation and everyday use, start with the [README](README.md).

## Reduction and repeatability

The search verifies a normalized wrong-answer baseline, then repeats contiguous-
chunk removal, simplification of each distinct value everywhere, and individual
simplification of repeated values. A full round with no acceptance establishes
local irreducibility and returns status 0, including normal no-acceptance results
and results with inconclusive checks. This does not promise the smallest failing
input or preservation of the same underlying bug.

Use `--max-evaluations 0` to verify and save only the baseline, or a positive
budget to bound actual candidate evaluations. Invalid candidates and the baseline
do not consume that budget. An unfinished search returns status 4 at its limit;
completion already established at the evaluation limit still returns 0.

Removal starts with floor(length / 2), clamped to one, sweeps left to right, stays
at the same position after acceptance, and halves chunk size down to singles.
A shorter tail chunk is tested at the end of a sweep. Distinct values are visited
in first-occurrence order and repeated-element positions from left to right.
Value changes first try the permitted value closest to zero, then one unit toward
zero; an accepted unit step enables binary search toward the simplest value.
Negative values also try their representable positive counterpart. Arithmetic is
safe at signed-64-bit extremes. Binary search is a threshold heuristic and can
miss other smaller inputs for arbitrary failure predicates.

Completion means that, on the exact result, every applicable strictly smaller
single-element removal, all-occurrences value change, and individual change of a
repeated value to simplest/unit/counterpart was invalid, lost the wrong answer,
or was inconclusive. Distinct inconclusive serialized candidate texts count once
per final round, separately from actual execution counts. Interrupted rounds are
labelled partial; baseline-only runs report no round. Completed search facts
survive a final-save error, which still requires exit status 1.

For stable deterministic programs, unchanged version, inputs, constraints and
options produce the same result bytes and accepted-input sequence. Time-limit or
interrupted runs, near-timeout executions, and different versions are excluded.
Without an overall budget, search continues until completion, cancellation, or
operational error, assuming terminating candidate processing and executions; no
practical total-runtime bound is promised.

## Evaluation cache

Each invocation caches only ordinary lost/inconclusive candidate outcomes in
memory. Fingerprints index candidates, but exact normalized text decides reuse,
so collisions cannot skip preserving candidates. Accepted inputs, operational
errors and unfinished evaluations are never cached. FIFO eviction bounds retained
candidate text to 8 MiB and entries to 4,096; oversized candidates execute without
being cached. The cache stores normalized identities and classifications, without execution
evidence. It does not persist across invocations or resume.

Cache hits and invalid candidates consume no evaluation budget. Actual execution
counts remain separate from distinct inconclusive texts in the current/final
round, including cached outcomes. Completed runs with caching disabled or entries
evicted retain the same result and accepted-input sequence; budget-limited runs
can stop at different inputs because they perform different amounts of work.
Cache disable/size/fingerprint controls are internal test controls; there is no
public cache flag.

## Development and CI

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
The same archive-based checks also run on the macOS 26 ARM64 compatibility runner,
including source Homebrew installation. The `source_installation` CTest check
builds and installs a CLI-only Release, checks the installed file inventory, and
runs both demonstrations outside the build directory. A manual workflow dispatch
against an existing approved tag repeats these checks on that exact tagged source
and tests the stable Homebrew formula; see [RELEASING.md](RELEASING.md).

## Input and CLI validation

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
A zero candidate budget still verifies the baseline exactly once, using only its
normalized layout. Both programs must succeed, disagree on exact output tokens,
and finish cleanup. Agreement or unsuccessful execution returns status 3 without
changing the output. Runner, cleanup, save, and callback errors return status 1;
overall time limits return 4 and Ctrl-C returns 130 unless an operational error
also occurred. No verified recovery input exists before successful verification.

Input and output must be regular files (a new output is allowed), and the output
parent must exist. Input and executable symlinks are permitted; output symlinks
are rejected. Output may not alias the original input or either executable,
including through hard links or symlinks in protected paths or parent directories.

## Program execution

Internal candidate evaluation accepts already normalized bytes, two explicit
`ProgramSpec` paths with ordered arguments, optional positive per-program timeouts, and a
caller-owned `CancellationToken`. Programs inherit the working directory,
environment, and intended signal state. They must create no descendants. The
runner installs no application-wide signal handlers and intercepts cancellation
and deadline waits at intervals no longer than 5 ms when scheduled.

Solution and reference timeouts are absent by default in both the CLI and typed
`RunLimits`. The overall elapsed-time and candidate-evaluation budgets are also
absent by default. An execution may wait indefinitely for a program to finish;
Ctrl-C/caller cancellation, a configured overall deadline, or an operational
failure can stop it. `--max-evaluations` limits candidate evaluations, not the
duration of an active execution.
This follows the approved [process-execution amendment](https://github.com/alifrahman2023/test-case-shrinker/issues/6#issuecomment-6509467180).

Set `--solution-timeout` or `--reference-timeout` to opt into a positive finite
deadline for that program. Each enabled deadline starts immediately before launch
and covers setup, input delivery, execution, and output collection using the
sleep-inclusive clock. A deadline during pending setup remains an operational
error; after setup it is an unsuccessful timeout. One program's timeout can be
configured independently of the other's. Cleanup retains its shared one-second
bound after a stop request even when execution deadlines are absent.

Each program retains at most 8,388,608 unchanged stdout bytes (one extra byte
fails execution) and the first 65,536 stderr bytes (extra bytes are drained and
marked). `Evaluation` owns its output and records disagreement using byte offsets
and a one-based token position; end-of-output is explicit. It has no ordinary
outcome on cancellation or operational error. Token scanning and comparison
also check cancellation in bounded chunks. Exit/signal facts remain separate
from timeout, overflow, cancellation, and peer-stop reasons. A completed wrong
answer requires zero exits, complete output, and successful cleanup for both.

## Runner ownership and lifetime

`Runner` is caller-owned and allows one synchronous evaluation or cleanup
operation at a time. Concurrent calls and reentry are unsupported, except
cancellation requests.
Hosts must not independently reap its children or install incompatible SIGCHLD
handling. Required stops use immediate SIGKILL and a shared maximum one-second
cleanup wait. If cleanup remains unresolved, the result reports a typed error
and known process facts, `has_pending_ownership()` remains true, and further
evaluations are refused. The runner retains the actual process/descriptor
ownership. Uncertain descriptor release also stays pending, without blindly
retrying a descriptor number that might have been reused. Diagnostic process identifiers never authorize caller signaling or
reaping. Destroying a runner with pending ownership terminates as a caller
contract violation, without an indefinite destructor wait. A continuing host
retains and services pending ownership with
`pending_ownership()` and `cleanup(wait, cancellation)`. Inspection returns owning
diagnostics for each unresolved role: known process identifier, whether reaping
is pending, open descriptor count, and uncertain-release count. Diagnostics do
not transfer process or descriptor ownership.

Each cleanup attempt retries termination and servicing of existing resources,
without launching programs. Its shared sleep-inclusive wait is between zero and
1,000 ms (default 1,000); zero makes one nonblocking pass. Cancellation interrupts
waiting while retaining unresolved work. `CleanupResult::resolved` reports full
release, `errors` reports that attempt's errors, and `execution` owns the latest
facts/output plus the original evaluation errors. Recovery never turns the
original failed evaluation into a successful candidate. Once ownership fully
resolves, new evaluations are admitted. A failed close is not retried blindly:
read-only inspection can establish that a descriptor is gone; an occupied,
possibly reused number remains uncertain.

Bounds promise prompt action when shrink can run, without a hard real-time,
kernel-operation, callback, or scheduling bound. The CLI may report an error and
exit; users need not keep it open indefinitely. CLI exit or a hard kill cannot
guarantee continued cleanup or termination of unresolved children. No background
manager, process-tree traversal, descendant detection, or sandbox is provided.
Supplied programs must create no descendants; threads remain permitted under
the determinism prerequisite. The CLI uses this evaluator for baseline verification.

Run the native execution and controlled clock/event groups with:

```sh
build/debug/shrink_core_tests '[runner],[execution-events]'
```

## Saving, evidence, and recovery

Verify and save a baseline (status 4 means the requested evaluation limit):

```sh
shrink --input failing.txt --solution ./solution --reference ./reference \
  --min-length 0 --max-length 100000 --min-value=-1000000000 \
  --max-value 1000000000 --max-evaluations 0 --output reduced.txt
```

The CLI always supplies saving. After verification it saves the normalized
baseline, marks accepted revisions pending, and checkpoints the latest best between
evaluations once at least one second has elapsed since the previous successful
save. Multiple acceptances coalesce; unchanged input is not periodically rewritten.
This throttles I/O and does not promise a maximum recovery gap. Initial or periodic
save failures stop search and retain their error even after a successful final
retry. The CLI attempts exactly one final save on stopping, even when unchanged
or the initial save failed. Each save writes a temporary file beside the output
and renames it over the destination, without fsync. Write/replacement failure
leaves the previous destination intact; this does not promise power-loss or OS
crash durability. Any save failure remains an operational error after a successful
final retry. Final-save failure attempts the complete normalized best input on
stdout and reports file and stdout-recovery errors on stderr. Ordinary stdout
otherwise stays empty.

Baseline and final evidence show both successful executions, the first differing
token, and escaped bounded token previews (including end of output). Evidence is
retained from verification; no extra final evaluation is performed. Failed
baselines show relevant exit/signal/stop facts and escaped stderr previews up to
4 KiB, separately marking the runner's 64 KiB retention truncation. Progress appears on stderr at most once per second and includes candidate
evaluations, acceptances, best length, and elapsed time; individual acceptances
are not printed. Quiet mode suppresses only periodic progress and retains
evidence, summary, and errors. A baseline-only result reports no round; interrupted searches report a partial
round. Completed results state the supported local guarantee and final-round
inconclusive count.

Resume by supplying the saved input, all required program/constraint options,
and a different output destination, for example `--input reduced.txt --output
reduced-again.txt --max-evaluations 0`. This performs fresh verification and
restores no search/cache state. Input/output aliases are rejected even when the
output is the default `reduced.txt`.

## Core results and callbacks

The internal synchronous `reduce` boundary takes an owning `ReductionRequest`, a
caller-owned `Runner`, a cancellation token, and optional `RunHooks`. Results own
baseline/best evidence, typed errors, counts, observed stop reasons, and saving
state; no CLI exit integers cross that boundary. Omitting the save hook returns
an explicitly unsaved in-memory result. Initial/periodic/final save requests include the
normalized bytes, revision, and save kind. Callback objects live through the
call, run on the calling thread, return promptly, and must not reenter the
runner. Borrowed arguments live only through that callback; copy retained data.
Baseline, acceptance, and throttled progress notifications run on the calling
thread; acceptance events expose the current accepted input. Results retain
baseline/current-best evidence and operational-failure evidence, with no ordinary
candidate execution history. Statistics count actual lost/inconclusive executions
and distinct inconclusive texts for the complete/partial final round.
Exceptions become typed operational errors; a throwing notifier is disabled,
and save exceptions obey the same single-final-attempt rule. Blocked callbacks
and filesystem operations can delay stopping. The overall sleep-inclusive budget
starts immediately before verification and includes callback/save time; cleanup
and final saving can extend beyond it.

## Overall budgets and cancellation

`--time-limit` accepts a positive whole duration with `ms` or `s`; no overall
limit is enabled by default. Timing starts after request validation and input
normalization, immediately before baseline execution. Native timing uses
`mach_continuous_time`, which includes computer sleep and is unaffected by
calendar-clock adjustments. The elapsed budget includes candidate generation,
validation, cache work, program execution and initial/periodic saves. Observing
elapsed time equal to the limit expires unfinished work, which is discarded
without acceptance or caching.

Overall expiry stops both active programs through the runner's normal bounded
cleanup. It remains distinct from caller cancellation and per-program timeouts,
including pending-setup errors. Before verification it returns status 4 with no
verified recovery input and leaves the destination unchanged. After verification
it retains the best and attempts one final save, with partial/no-round counts
and no irreducibility claim. Operational errors require status 1 while retaining
observed time/cancellation reasons. Cleanup and final saving may exceed the
budget; completed-search guarantees survive expiry during final saving. These
are prompt checks when scheduled, without hard callback, filesystem or kernel
operation bounds.

Ctrl-C handling belongs to the terminal adapter, which records SIGINT in a
signal-safe flag and checks it from ordinary code. Supplied programs run in
separate process groups, retaining their intended signal masks/dispositions;
the runner terminates owned direct children with immediate SIGKILL and shares
one bounded cleanup wait. No descendant signaling or cleanup is added. Before
verification, cancellation leaves the destination unchanged and emits no
recovery input. After verification, cancellation retains the best known failing
input and still attempts one final save, including when Ctrl-C arrives during
saving. Status 130 denotes cancellation; observed cleanup/save/notifier errors
require status 1 while preserving the cancellation reason and verified recovery
state. Pending ownership remains explicit, and process exit cannot guarantee
continued cleanup. Search checks cancellation during generation/validation too, including
invalid candidates that never launch programs. Unfinished work is discarded.
