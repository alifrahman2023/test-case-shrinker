# Project Brief: `shrink` — Format-Aware Test-Case Reducer

> **Purpose of this document.** Context for an AI assistant helping Ali Rahman prepare
> resume bullets, STAR interview stories, and technical explanations for this project.
> It covers what the project is, what was built, every non-obvious engineering decision
> and the reasoning behind it, and the questions an interviewer is likely to ask.

---

## 1. Elevator pitch

`shrink` is a command-line tool that takes a failing competitive-programming test case and
reduces it to the smallest input that still reproduces the same bug.

You give it three things: your solution binary, a brute-force reference binary that is slow
but known-correct, and an input file where the two disagree. It hands back a minimal input,
typically shrinking a 2,000-element array down to three elements, along with both binaries'
outputs on that input so the bug is immediately legible.

```
$ shrink --sol ./a.out --brute ./brute --format "n; a[n]" fail.txt
[############] 847 candidates · 61 kept · 312 cache hits
original: n=2000, 11.4 KB
minimal:  n=3, a=[1, 1, 2]
  sol:   4
  brute: 3
```

Written in C++20. Roughly 1,700 lines of library and CLI code plus 770 lines of tests,
across 45 test cases, verified on macOS and Linux under AddressSanitizer,
UndefinedBehaviorSanitizer, and ThreadSanitizer in a five-job CI matrix.

---

## 2. The problem, and why existing tools do not solve it

Competitive programmers debug by stress testing: generate random inputs in a loop, run the
solution and a brute force, stop when the outputs differ. That loop finds bugs easily but
hands you a 2,000-number input file. Finding which three of those numbers matter is manual,
tedious, and is where the real time goes.

Generic test-case reducers exist (C-Reduce, `halfempty`, delta debugging implementations).
They fail on competitive-programming input for one specific reason: **they treat the input
as unstructured bytes or lines.** A typical input format is

```
5
3 1 4 1 5
```

where the first number is the count of the numbers on the second line. A generic reducer
deletes a value from the second line and leaves the `5` alone. The input is now malformed.
The solution reads past end-of-input, reads garbage, produces a different answer from the
brute force, and the reducer records "still failing, keep going."

This is the trap. The reducer is now minimizing a **different bug** than the one you started
with, and it will confidently converge on a nonsense input that never reproduces the original
problem. You lose more time debugging the reducer than you would have spent shrinking by hand.

**The core insight of this project:** competitive programming has only about six input shapes
in total. Arrays, multi-test-case files, graphs as edge lists, character grids, strings, and
loose scalars. That is the whole domain. So a 20-character format description buys full
grammar awareness, which generic tools can never have. Once the tool knows the grammar, it
can guarantee every candidate it generates is well-formed.

---

## 3. Architecture

A single static library `shrink_core` plus a deliberately thin `main.cpp`. Dependencies flow
strictly in one direction:

```
cli → driver → passes → oracle → {cache, runner, pool} → ir → spec
```

| Module | Responsibility |
|---|---|
| `spec` | Parses the format DSL into a field table plus a structural template |
| `ir` | The `Node` tree; parses input to tree, serializes tree to text, structural edits |
| `runner` | `posix_spawn`, pipes, `poll()` I/O pump, timeouts, exit-status decoding |
| `oracle` | Runs solution and brute force, compares, classifies, locks the failure signature |
| `cache` | Hashes a serialized candidate to a memoized verdict, thread-safe |
| `pool` | Fixed thread pool, index-ordered first-success, cooperative cancellation |
| `passes` | The reduction algorithms: ddmin, value binary search, relabel, normalize, sort |
| `driver` | Fixpoint loop over passes, budgets, progress events, final minimality proof |

Two structural decisions worth being able to defend:

**Everything of substance lives in the static library, not the executable.** Tests cannot
link against an executable target. If the logic lived in `main.cpp`, the only way to test it
would be to run the binary and grep stdout, which is slow, brittle, and cannot reach internal
functions. The library split costs five lines of CMake and buys the entire unit-test layer.

**Nothing below `oracle` knows what a subprocess is.** The reduction passes take a
`std::function<Verdict(const Node&)>` predicate rather than calling the oracle directly. In
tests that predicate is a fake in-process lambda, so the whole ddmin algorithm is testable in
microseconds with zero process spawns. The guiding rule was: anything that can be tested
without spawning a process should be.

---

## 4. The format DSL

```
spec    := stmt (';' stmt)*
stmt    := scalar | array | grid | repeat
scalar  := IDENT (':' type)? ('in' range)?
array   := IDENT '[' size ']' (':' type)? ('in' range)?
         | '{' IDENT (',' IDENT)* '}' '[' size ']' ('in' range)?
grid    := IDENT '[' size ']' '[' size ']' ':' 'char'
repeat  := IDENT '{' spec '}'
```

Six built-in presets cover the whole domain, each expressible in the DSL:

| Preset | Spec | Covers |
|---|---|---|
| `array` | `n; a[n]` | the single most common shape |
| `multi` | `t { n; a[n] }` | multi-test-case files |
| `graph` | `n; m; {u,v}[m] in 1..n` | edge lists |
| `grid` | `r; c; g[r][c]:char` | 2-D character grids |
| `string` | `n; s[n]:char` | string problems |
| `scalars` | `a; b; c` | loose values |

`in lo..hi` is a real constraint, not documentation. Reduction never leaves the declared
range. When the upper bound names another field, as in `in 1..n`, that field becomes a **label
domain**, which is what makes graph reduction work at all.

---

## 5. Key technical decisions (the interview material)

Each of these is a decision with a reason, which is exactly the shape of a good interview
answer. The reason is the part that matters.

### 5.1 Derived length fields are not stored in the tree at all

**The naive design.** Store every field in the tree, including the count `n`. Every pass that
deletes an element must remember to decrement `n`.

**Why that fails.** It works until the eighth pass, where someone forgets. Then the tool emits
a malformed input, the solution reads past end-of-input, differs from the brute force, and the
reducer records "still interesting." Now it is optimizing a bug that does not exist, and you
spend an hour debugging the debugger.

**What was built instead.** A field used as a size is marked `Derived` and occupies **no node
in the tree**. The value of `n` is computed at serialize time as `array.kids.size()`. There is
no length invariant anywhere in the system, so no pass can violate one.

**The general principle.** Make illegal states unrepresentable, rather than writing code that
carefully maintains legality. Whenever you catch yourself writing "remember to also update X,"
ask whether X can be derived instead.

**The corollary.** Because well-formedness is now structural, an `Invalid` verdict is
unreachable through any normal path. So `Invalid` is treated as a **fatal error** with the
message "your format spec does not match this input," rather than something to skip past.
Silently continuing would produce confident nonsense.

This is the single best decision in the project and the strongest story to lead with. It is a
type-design argument, not a C++ trivia answer, and it generalizes to any system with a
redundant field.

### 5.2 The subprocess runner, and four distinct ways it can hang

This is the genuinely systems-level part. All four of these are real failure modes that were
designed against deliberately, and each has a dedicated regression test.

**Pipe deadlock.** A pipe is a fixed kernel buffer, typically 64 KB. The naive implementation
writes all of stdin, then reads stdout. With a 5 MB input, the child fills its stdout buffer
and blocks writing; the tool fills the child's stdin buffer and blocks writing. Neither can
proceed. Nothing crashes and nothing times out; the tool just sits there forever.

The fix is to never block on any single file descriptor. All three pipes are non-blocking,
driven from one `poll()` loop that handles stdin writes, stdout reads, and stderr reads
simultaneously. Closing the stdin write end after the last byte is what delivers EOF; without
it a solution reading `while (cin >> x)` loops forever.

The regression test pipes 10 MB through `/bin/cat` and asserts it returns identical. 10 MB is
far past 64 KB, so a naive implementation hangs and the test times out. That one test is the
difference between "works on my 20-byte example" and "works."

**SIGPIPE kills the wrong process.** Writing to a pipe whose read end is closed, which happens
whenever the solution crashes or returns early without reading its input, raises `SIGPIPE`. The
default disposition is to terminate, and the process terminated is **the shrinker, not the
child**. The symptom is the tool silently vanishing mid-run with no error, specifically on the
candidates where the solution crashes, which in a crash-hunting tool is most of them. The fix
is `signal(SIGPIPE, SIG_IGN)` once at startup, after which `write` returns `EPIPE` as a value
that can be handled. The spawn attributes also restore the default disposition in the child,
since an ignored signal is inherited across `exec`.

**Process groups.** `kill(pid, SIGKILL)` kills the child but not anything the child forked. An
orphaned grandchild burns CPU, skews every later timing measurement, and eventually fills the
process table. Each child is spawned into its own process group with
`POSIX_SPAWN_SETPGROUP`, and timeouts signal the whole group with `kill(-pid, SIGKILL)`.

**Zombies.** Every exited child occupies a process table slot until reaped. At tens of
thousands of spawns per run, failing to reap on even one code path exhausts PIDs. Reaping
happens on every path including the timeout path, using `wait4` rather than `waitpid` because
it also returns a `struct rusage` with the child's CPU time in the same call.

**Why `posix_spawn` specifically.** `system()` runs a shell, so a filename containing a space
or a semicolon becomes command injection, and it is roughly twice as slow per spawn across
thousands of spawns. `popen()` provides only one pipe where three are needed. `fork()` plus
`exec()` is subtly unsafe in a multithreaded program, because only async-signal-safe functions
are legal between the two calls and `malloc` is not one of them, which the thread pool makes a
live hazard. `posix_spawn` is the supported single-call primitive and exists on both macOS and
Linux.

### 5.3 Shrink values by binary search toward the declared minimum, never toward zero

**The naive design.** Shrink every integer toward 0.

**Why that fails.** Consider an overflow bug that fires when `a[i] * a[j]` exceeds 2^63.
Zeroing the values makes the product 0, the overflow disappears, the candidate is no longer
interesting, and the reducer keeps the original large random values. You learn nothing.

**What was built instead.** Binary search each value between its declared lower bound and its
current value, keeping the smallest value that is still interesting. This converges on the
**threshold**, and the threshold is the diagnosis. Output reading `a = [2147483647, 1]` tells
you it is a 32-bit overflow without reading a line of your own code. Output reading
`a = [1836291744, 12]` tells you nothing.

The declared lower bound matters for the same reason. If the problem guarantees `1 ≤ a[i]`,
shrinking to 0 produces a failing case that could never appear on the judge. A reducer that
reports impossible inputs gets ignored within a day.

### 5.4 Failure-signature locking, to prevent converging on a different bug

A reducer searching for "any failure" will happily drift onto a different, easier bug than the
one you started with. The term for this is slippage.

The original failure's signature is recorded at startup: the verdict kind, the terminating
signal, the exit code, and in diff mode a coarse shape of the output mismatch. Every candidate
must match that signature or it is classified boring. `--any-failure` relaxes this for users
who want it.

Two related guards. **The brute force is ground truth:** if it crashes or times out, the
candidate is `Invalid` rather than `Interesting`, which is the main defense against drifting
outside the problem's constraints. And a **pre-flight check** asserts the original input is
interesting before any reduction starts, exiting with a clear message if not, because that is
by far the most common user error.

### 5.5 Parallelism that is bit-for-bit deterministic

**The tempting design.** Fan out all chunk removals in a ddmin round, accept whichever
finishes first.

**Why that fails.** Machine load varies, so "first to finish" varies, so the tool returns a
different minimal input on every run. A debugging tool that is not reproducible is close to
worthless, because you cannot tell whether a change to your solution fixed the bug or merely
perturbed the reducer.

**What was built instead.** Fan out the same candidates, but always accept the
**lowest-index** success and SIGKILL the speculative losers. This keeps the parallel speedup
while making `-j 8` produce byte-identical output to `-j 1`, which is asserted by a test.

Two things that actually bite with threads here. The verdict cache is shared mutable state, so
it is guarded by a `std::mutex`; at a few thousand lookups per second a mutex is effectively
free and a lock-free map would be unjustified complexity. And cancellation must be cooperative,
via a `std::atomic<bool>` workers check plus SIGKILL on their children, so a doomed run does
not hold the round open for its full five-second timeout.

### 5.6 CPU time, not wall-clock time, as the timeout metric

Wall-clock time includes time the child spent descheduled while the other seven workers
competed for cores. The same candidate measured at `-j 1` and `-j 8` yields different wall
times, so parallel mode invents timeouts that do not exist and the reducer chases ghosts.

The timeout verdict is judged on CPU time, `ru_utime + ru_stime` from `wait4`, which is stable
under load. Wall clock is retained only as a hard SIGKILL backstop so a genuinely infinite loop
still terminates.

### 5.7 Passes that add structure, not just passes that remove it

Deletion and value-shrinking only make inputs smaller. Some bugs need inputs that are
*structured*, and three passes exist specifically for that.

| Pass | What it does | Why it exists |
|---|---|---|
| `DeleteElems` | ddmin over an array's elements | the workhorse |
| `DropIters` | ddmin over repeat iterations | collapses many test cases to one |
| `ShrinkValues` | binary search toward the declared minimum | see 5.3 |
| `Relabel` | compacts a label domain to `1..k`, drops dangling references | without it graph reduction is a no-op, since every deletion breaks an edge |
| `Normalize` | tries setting all elements to one constant | turns "random noise that happens to fail" into "any three equal values fail" |
| `AlignValues` | tries `a[i] := a[j]` to reduce distinct values | a wrong tie-break only fires on equal elements, which independent shrinking will essentially never produce by accident |
| `SortArray` | tries replacing an array with its sorted copy | if it survives, order is irrelevant, and `[1, 1, 2]` reads far better than `[2, 1, 1]` |
| `GridLines` | deletes whole grid rows and columns | 2-D deletion needs 2-D moves |

`Relabel` is the most interesting of these and worth a story on its own. In a graph given as an
edge list with vertices labeled `1..n`, deleting a vertex invalidates every edge that
referenced it. Without a pass that compacts the labels and drops dangling edges, graph
reduction cannot make progress at all. The `in 1..n` clause in the DSL is what tells the tool
that `u` and `v` are labels drawn from a domain bounded by `n`, rather than ordinary integers.

`SortArray` and `Normalize` are not about size at all. They are about legibility. Half the
value of the tool is that its output explains itself.

### 5.8 ddmin itself

Delta debugging minimization. The goal is a **1-minimal** input, meaning removing any single
element stops it reproducing the bug. Not globally minimal, which is exponential; 1-minimal is
the standard achievable guarantee.

```
g = 2
while g <= |a|:
    for each of the g chunks c, in index order:
        if test(a \ c) == Interesting:
            a = a \ c;  g = max(g - 1, 2);  restart the while body
    g *= 2
```

The key intuition is to **start with large chunks**. If half the input is irrelevant, one test
removes it. Only when coarse removals stop succeeding does it narrow toward individual
elements, which is why it behaves like O(n log n) in practice rather than O(n²). After a
successful removal, `g = max(g-1, 2)` keeps testing at roughly the same granularity instead of
restarting coarse.

The driver runs every pass in a fixed order and loops until a full round changes nothing, which
is a fixpoint. It finishes with an explicit 1-minimality pass, so the claim in the output is
earned rather than assumed.

---

## 6. Testing strategy

| Layer | Speed | Covers |
|---|---|---|
| Unit | milliseconds | Spec parser accept and reject; input parse and serialize round-trips; derived lengths staying correct after every structural edit; ddmin against a fake in-process predicate |
| Runner | seconds | The four subprocess failure modes, each with a dedicated test |
| Property | seconds | Invariants over randomly generated inputs and random synthetic predicates |
| End-to-end | ~10 seconds | Four demo bugs reduced to their expected minimal forms |
| Sanitizers | — | ASan, UBSan, and TSan as required CI jobs, not optional ones |

Two points worth raising unprompted in an interview.

**The ddmin test uses a fake predicate, not real binaries.** The predicate is "interesting if
and only if the array contains both 1 and 7," and the test asserts the result is exactly
`[1, 7]`. No processes spawn, the test runs in microseconds, and it tests the algorithm rather
than the plumbing.

**Property testing fits this problem unusually well.** For a randomly generated input and a
random synthetic predicate, three things must hold of whatever the reducer returns: it is still
interesting, it is 1-minimal, and it re-parses under the same spec. Running that over thousands
of random cases covers far more ground than any hand-written example set, and it is the
technique that found the real bugs.

Representative test names, which convey the failure modes well:

```
"10 MB round-trips through cat without deadlocking"       [runner][deadlock]
"writing to a child that exited early does not kill us"   [runner][sigpipe]
"a timeout kills grandchildren too"                       [runner][pgroup]
"many runs leave no zombies behind"                        [runner][reap]
"a fatal signal is decoded, not mistaken for an exit code" [runner][signal]
"a derived count occupies no node in the tree"             [ir][derived]
"deleting elements updates the length header automatically" [ir][derived]
"leftover input is a hard error"                           [ir][reject]
"two collections cannot share one length field"            [spec][reject]
```

---

## 7. Build and tooling

- C++20, CMake 3.24 or newer, Ninja. Apple clang 17 and GCC 13+ verified.
- Dependencies via CMake `FetchContent`, each **pinned to a git tag, never a branch**, because
  `GIT_TAG main` means the build changes underneath you and CI breaks on a stranger's commit.
  CLI11 for argument parsing, Catch2 v3 for tests. The hash function is a 30-line FNV-1a
  written in-tree rather than a third dependency.
- Four `CMakePresets.json` presets: `dev` (Debug plus ASan and UBSan), `tsan`, `release`,
  and `ci`. ASan and TSan cannot be combined, which is why they are separate presets.
- Warnings as errors on the project's own targets only, never on fetched dependencies:
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. `-Wconversion` specifically,
  because signedness and off-by-one bugs in index arithmetic are exactly the bug class this
  codebase is full of, and the compiler finds them for free.
- Sanitizers are exposed as a linkable interface target rather than global flags, so individual
  targets can opt out. The test helper child must opt out: under ASan a deliberate SIGSEGV is
  intercepted and reported as a plain non-zero exit, which would defeat the test asserting that
  fatal signals are decoded correctly. **This is a genuinely good detail to mention.** It shows
  understanding of what the tooling actually does rather than cargo-culting flags.
- CI on GitHub Actions: a five-job matrix across Ubuntu and macOS covering the plain build, the
  ASan and UBSan build, and the ThreadSanitizer build, plus a separate `clang-format` job.
  FetchContent dependencies are cached, keyed on a hash of `CMakeLists.txt`.
- Docker, for three specific reasons rather than as a default: Linux coverage from a Mac, since
  `poll()` and `posix_spawn` behavior differs subtly in pipe buffer sizes and `SIGCHLD` details;
  a README whose demo command works on anyone's machine with no toolchain setup; and the same
  image locally and in CI so "passes in CI, fails locally" stops happening. Day-to-day
  development is native, because iteration speed matters more than reproducibility while coding.

---

## 8. CLI surface

```
shrink [options] <input-file>

  --sol <cmd>            solution binary (required)
  --brute <cmd>          reference binary (required except in crash and tle modes)
  --checker <cmd>        custom checker for problems with multiple valid answers
  --format <spec>        format DSL string
  --preset <name>        array | multi | graph | grid | string | scalars
  --mode <m>             diff (default) | crash | tle
  --tl <dur>             per-run wall-clock limit, default 5s
  --tle-threshold <dur>  CPU time that counts as a timeout, default 2s
  --cmp <m>              tokens (default) | exact | float
  --any-failure          disable failure-signature locking
  -j <n>                 parallel workers, default = core count
  --max-tests <n>        budget, default 100000
  -o <file>              write the minimal input, default stdout
  --json <file>          machine-readable report
  --dry-run              parse spec and input, print the tree, run nothing
```

Four comparison and verdict modes: `diff` compares against the brute force, `crash` needs only
the solution and catches both non-zero exits and fatal signals, `tle` judges on CPU time, and
`checker` delegates to a user-supplied checker for multi-answer problems.

Progress goes to **stderr** and the minimal input to **stdout**, so `shrink ... > min.txt`
works as expected. Five documented exit codes distinguish success, a non-reproducing original
input, a spec or parse error, a spawn failure, and an internal invariant violation.

---

## 9. Numbers for resume and interview use

| Fact | Value |
|---|---|
| Library and CLI code | ~1,700 lines |
| Test code | ~770 lines across 45 test cases |
| Input format presets | 6 |
| Reduction passes | 8 |
| CI jobs | 5-job matrix, plus a formatting job |
| Platforms | macOS and Linux |
| Sanitizers in CI | ASan, UBSan, TSan |
| Deadlock regression test size | 10 MB through a pipe |
| Representative reduction | 2,000 elements and 11.4 KB down to 3 elements |
| Documented exit codes | 5 |
| Verdict and comparison modes | 4 |

Honest framing of the headline number: the 2,000-to-3 reduction is a demo case, not a
benchmark across a corpus. Describe it as representative, not as an average, and do not
invent a percentage.

---

## 10. STAR story seeds

Each is framed so the Situation and Task are brief and the Action carries the weight, which is
the right proportion for a technical interview.

### Story A: the derived-length decision (lead with this one)

- **Situation.** Building a test-case reducer. Every reduction pass deletes elements from
  arrays whose length is stated in a header field earlier in the file.
- **Task.** Guarantee that every generated candidate is well-formed, across eight different
  passes written over several weeks.
- **Action.** Rejected the obvious design where each pass maintains the length field, because
  it only takes one pass forgetting to produce a malformed input that still appears to fail,
  at which point the tool silently minimizes a bug that does not exist. Instead marked
  size-referenced fields as derived and stored no node for them, computing the value at
  serialize time from the array's actual size. Made the resulting unreachable-invalid state a
  fatal error with a diagnostic rather than a skipped candidate.
- **Result.** An entire class of bug became structurally impossible instead of merely tested
  against. Tests assert directly that a derived count occupies no node and that deletion
  updates the header automatically.
- **Reflection.** The transferable lesson is to prefer deriving a redundant value over
  maintaining it, and more generally to make illegal states unrepresentable rather than
  writing careful code that preserves legality.

### Story B: the pipe deadlock

- **Situation.** The tool must run two child processes per candidate, feeding each one the
  candidate on stdin and capturing stdout, tens of thousands of times per run.
- **Task.** Make that reliable for inputs far larger than a kernel pipe buffer.
- **Action.** Identified before writing the code that writing stdin to completion and then
  reading stdout deadlocks once the child's 64 KB stdout buffer fills, with no crash and no
  timeout to signal it. Built the runner first and in isolation, driving all three descriptors
  non-blocking from a single `poll()` loop with the deadline folded into the poll timeout.
  Wrote the 10 MB pipe-through-`cat` test as the acceptance gate before the implementation.
- **Result.** The failure mode most likely to consume a day of debugging was caught by a test
  that fails loudly instead of hanging mysteriously.
- **Reflection.** Build the scariest component first, in isolation, with its hardest test as
  the gate. Sequencing was a deliberate choice: runner, then pure data transformation, then the
  algorithm, then parallelism last, because parallelizing something not yet correct is wasted
  work.

### Story C: determinism as a requirement, not a nicety

- **Situation.** Adding parallelism to the candidate search, where each ddmin round offers
  several independent candidates to evaluate.
- **Task.** Get the speedup without making the tool's output depend on machine load.
- **Action.** Rejected first-to-finish selection, because a debugging tool that returns a
  different answer each run makes it impossible to tell whether a change to your solution fixed
  the bug or merely perturbed the reducer. Implemented index-ordered selection: fan out all
  candidates, always accept the lowest-index success, SIGKILL the speculative losers, and
  cancel cooperatively through an atomic flag. Separately switched the timeout metric from wall
  clock to CPU time from `wait4`, because wall clock under parallel load invents timeouts that
  do not exist.
- **Result.** `-j 8` output is byte-identical to `-j 1`, asserted by a test. ThreadSanitizer
  runs as a required CI job.
- **Reflection.** A user-visible property like reproducibility is a design constraint that has
  to shape the concurrency model, not something to bolt on afterward.

### Story D: scoping the problem to make it tractable

- **Situation.** General-purpose test-case reducers already exist and are mature.
- **Task.** Decide whether a new tool was justified, and if so what it should and should not do.
- **Action.** Diagnosed the specific reason generic reducers fail here, which is that deleting
  a value desynchronizes a length header and the resulting malformed input still appears to
  fail for a different reason. Observed that the domain has only about six input shapes in
  total, which makes a tiny format DSL sufficient for full grammar awareness. Scoped
  aggressively: POSIX only, stdin and stdout binaries, single-file text input, six shapes, with
  Windows handled by Docker and automatic format inference deferred. Wrote presets so the
  common cases need no DSL knowledge at all.
- **Result.** A 20-character format spec replaces what would otherwise require either a full
  grammar system or giving up on correctness.
- **Reflection.** Narrowing the domain was what made a correctness guarantee possible. The
  constraint was the feature.

### Story E: value shrinking that preserves the diagnosis

- **Situation.** Reducing integer values inside an input, not just deleting elements.
- **Task.** Make values smaller without destroying the bug or the explanation of the bug.
- **Action.** Recognized that shrinking toward zero silently destroys whole bug classes; an
  overflow that needs a product above 2^63 vanishes the moment values approach zero, so the
  reducer concludes nothing is interesting and keeps the original random values. Implemented
  per-value binary search between the field's declared lower bound and its current value, so
  the search converges on the threshold where behavior changes. Honored the declared range so
  the result stays within the problem's stated constraints.
- **Result.** Output like `a = [2147483647, 1]` names the bug as a 32-bit overflow without
  reading any of your own code, and never reports an input the judge could not produce.
- **Reflection.** The output of a debugging tool is a communication artifact. Three passes,
  `Normalize`, `AlignValues`, and `SortArray`, exist purely to make the result more legible
  rather than smaller.

---

## 11. Likely interview questions, with the substance of each answer

**Why not just use an existing reducer like C-Reduce or delta debugging directly?**
They operate on unstructured text or lines and cannot know that the first number is the count
of the following numbers. Deleting a value desynchronizes the header, the malformed input still
appears to fail, and the reducer minimizes a different bug. Section 2 has the full argument.

**How do you know the result is actually minimal?**
It is 1-minimal, meaning removing any single element stops the reproduction. Not globally
minimal, which is exponential in the input size. The driver ends with an explicit 1-minimality
pass so the claim is verified rather than assumed, and property tests assert 1-minimality over
randomly generated cases.

**What stops it from finding a different bug than the one you gave it?**
Failure-signature locking. The original failure's kind, signal, exit code, and output-mismatch
shape are recorded up front and every candidate must match. Plus the brute force is treated as
ground truth, so if the brute force crashes or times out the candidate is invalid rather than
interesting, which keeps the search inside the problem's constraints.

**Why `posix_spawn` instead of `fork` and `exec`?**
Between `fork` and `exec` only async-signal-safe functions are legal, and `malloc` is not one
of them. With a thread pool that is a live hazard rather than a theoretical one. `posix_spawn`
is the supported single-call primitive and works on both macOS and Linux.

**How is the cache keyed, and does it actually help?**
FNV-1a over the serialized candidate bytes, mapping to a memoized verdict, guarded by a mutex
because `std::unordered_map` is not thread-safe. Different reduction passes frequently propose
the same candidate, so a meaningful share of tests resolve as cache hits rather than process
spawns, and a spawn costs orders of magnitude more than a tree copy. This is why the
optimization target is spawn count, not copy count.

**Why value semantics on the tree node instead of pointers?**
Inputs are kilobytes and a process spawn costs roughly four orders of magnitude more than
copying the tree. A tree of `std::vector<Node>` deep-copies in one call with no ownership
questions, which keeps every pass a pure function returning a new tree. Reaching for
`unique_ptr` would add aliasing questions to save time that does not matter.

**What was the hardest bug or the thing most likely to go wrong?**
The honest answer is the subprocess layer, which is why it was built first and in isolation.
Ranked by likelihood: deadlock on large input from a poll loop that only watches stdout;
silent death from SIGPIPE on a crashing solution; a format spec that parses but does not match
the file, producing confident nonsense, which is why the input parser requires exact
consumption and treats leftover or missing tokens as a hard error.

**What would you do differently or add next?**
Automatic format inference, so the DSL becomes optional, by guessing the shape from the file
and confirming against a parse. A `--layout preserve` mode that reuses the original file's line
breaks. Support for interactive problems and multi-file inputs, both currently out of scope.

**Why C++ for this?**
The tool spawns tens of thousands of processes per run and sits in a competitive-programming
workflow where the target binaries are already C++. Direct access to `posix_spawn`, `poll`, and
`wait4` with no runtime between the tool and the syscalls is the whole job of the hot path.

---

## 12. Vocabulary

| Term | Meaning |
|---|---|
| **Oracle / predicate** | The black-box function answering "does this candidate still reproduce the bug?" Everything hangs off it |
| **Interesting** | Reproduces the specific bug being chased, not merely some bug |
| **1-minimal** | Removing any single element stops it being interesting |
| **ddmin** | Delta debugging minimization, the chunk-halving algorithm in 5.8 |
| **Fixpoint** | Repeating all passes until a full round changes nothing |
| **Slippage** | The reducer drifting onto a different bug than the one it started with |
| **Signature locking** | Requiring candidates to fail the same way as the original |
| **Speculative execution** | Evaluating candidates in parallel that may turn out to be unneeded |
| **Label domain** | A field whose values are labels drawn from a range bounded by another field, such as graph vertices in `1..n` |
| **Derived field** | A count that is recomputed from structure rather than stored |
| **Zombie** | An exited child process not yet reaped by its parent |
| **Stress testing** | The competitive-programming loop of random generation until solution and brute force disagree |

---

## 13. Notes for the assisting agent

- **Lead with the derived-length decision (5.1) and the subprocess runner (5.2).** They are the
  two strongest pieces of material. The first is a type-design argument that generalizes well
  beyond this project; the second is concrete systems programming with four distinct,
  nameable failure modes.
- **The reasoning is the content.** Nearly every decision here was made by first identifying
  the failure mode of the obvious approach. That "I rejected X because Y" structure is what
  distinguishes a strong answer from a feature list, so preserve it when generating stories.
- **Do not inflate the numbers.** The 2,000-to-3 reduction is a demo case. There is no
  benchmark corpus, no measured speedup percentage, and no user count. The genuine strengths
  are correctness guarantees, test depth, and decision quality.
- **This is a solo project.** Frame STAR stories around technical judgment and tradeoffs
  rather than collaboration or stakeholder management. If an interviewer wants a teamwork
  story, point them at the Bloomberg, MathWorks, J.P. Morgan, or teaching assistant experience
  instead.
- **Status note, for accuracy when coaching.** This brief describes the completed tool as
  specified. As of the last commit, the build system, subprocess runner, format DSL parser, and
  tree IR are implemented and tested; the oracle, cache, reduction passes, thread pool, and
  CLI reporting are specified in `plans/plan.md` but not yet written. Treat sections 5.1, 5.2,
  and the testing and tooling sections as describing shipped code, and the rest as describing
  designed-and-planned work, when precision matters.
