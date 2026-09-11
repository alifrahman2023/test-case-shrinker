# Notes — the "why" behind [plan.md](../plans/plan.md)

Written for someone who hasn't built a C++ CLI tool with CMake, subprocesses and threads before.
Read this once end-to-end, then keep it open while you work through the milestones.

---

## 0. Suggested order to actually write this

Do **not** start at the CLI. Start at the scariest part while you're fresh:

1. `runner` (subprocess plumbing) — hardest, most likely to eat a day, and everything depends on it.
2. `spec` + `ir` — pure data transformation, easy to test, no I/O.
3. `oracle` — glues 1 and 2 together.
4. `passes` + `driver` — the actual algorithm. By now you can test it without spawning anything.
5. `pool` — parallelism last. Never parallelise something that isn't correct yet.
6. CLI, demos, README.

Rule of thumb: **anything that can be tested without spawning a process, should be.** That's why
the plan splits `oracle` (spawns processes) from `passes` (doesn't). The passes take a
`std::function<Verdict(const Node&)>`, so in unit tests you hand them a fake lambda like
`[](auto& n){ return contains(n,1) && contains(n,7) ? Interesting : Boring; }` and the whole
ddmin algorithm becomes testable in microseconds with zero processes.

---

## 1. CMake, explained just enough

CMake is a build-system *generator*: you describe targets, it emits Makefiles/Ninja files.

### Targets

A target is a thing you build. You'll have three:

```cmake
add_library(shrink_core STATIC src/spec.cpp src/ir.cpp src/runner.cpp ...)
add_executable(shrink src/main.cpp)
add_executable(shrink_tests tests/unit/spec_test.cpp ...)

target_link_libraries(shrink        PRIVATE shrink_core CLI11::CLI11)
target_link_libraries(shrink_tests  PRIVATE shrink_core Catch2::Catch2WithMain)
```

**Why a library plus a tiny `main.cpp`, instead of one big executable?** Because tests can't link
against an executable. If all your logic lives in `main.cpp`, the only way to test it is to run
the binary and grep its output — slow, brittle, and it can't test internal functions. The library
split costs you five lines of CMake and buys you the entire unit-test layer. This is the single
most important structural habit in C++ projects.

### PUBLIC / PRIVATE / INTERFACE

These control *propagation*, not visibility:

- `PRIVATE` — needed to build this target, not by things that link it.
- `PUBLIC` — needed by both.
- `INTERFACE` — needed only by consumers (header-only libs).

`target_include_directories(shrink_core PUBLIC include/)` means: anything linking `shrink_core`
automatically gets `include/` on its include path. That's why `shrink_tests` can just
`#include <shrink/ir.hpp>` with no extra config.

Prefer `target_*` commands over the global ones (`include_directories`, `add_definitions`).
The global ones leak settings into every target in the directory and are considered legacy.

### FetchContent

Downloads and builds a dependency as part of your build. No system packages, no vcpkg, works
identically on your Mac and in CI:

```cmake
include(FetchContent)
FetchContent_Declare(catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1)          # pin a tag, never a branch
FetchContent_MakeAvailable(catch2)
```

Pin the tag. `GIT_TAG main` means your build changes under you and CI breaks on someone else's
commit.

### Presets

`CMakePresets.json` saves you from remembering flag soup. Instead of a 200-character
`cmake -B build -DCMAKE_BUILD_TYPE=...` line you type `cmake --preset dev`. Define at least:

- `dev` — `Debug`, `-fsanitize=address,undefined -fno-omit-frame-pointer`
- `tsan` — `Debug`, `-fsanitize=thread` (ASan and TSan can't be combined; separate preset)
- `release` — `-O2 -DNDEBUG`

### Sanitizers

Free bug detectors built into clang. Turn them on for your dev builds and leave them on.

- **ASan** — use-after-free, buffer overflow, leaks. Slows you ~2×.
- **UBSan** — signed overflow, bad shifts, misaligned loads, invalid enum values.
- **TSan** — data races. You *will* need this once the thread pool exists. Race conditions are
  invisible until they aren't; TSan finds them deterministically.

A test suite that passes under ASan is worth several times one that passes without it.

---

## 2. The subprocess part (this is the real C++ content)

Most of this project is ordinary data structures. The genuinely systems-y part is running two
child processes safely, and it has four classic ways to hang or crash. Understand all four before
writing the code.

### 2.1 Why `posix_spawn` and not `system()` / `popen()`

- `system("./a.out < in.txt")` runs a shell. The shell parses your arguments, so a filename with
  a space or a `;` becomes a command-injection bug. It's also ~2× slower per spawn, and you'll do
  thousands of spawns.
- `popen()` gives you *one* pipe. You need three (stdin, stdout, stderr) at once.
- `fork()` + `exec()` works but is subtly dangerous in a multithreaded program: between fork and
  exec, only async-signal-safe functions are legal, and `malloc` isn't one of them. Your thread
  pool makes this a live hazard.
- `posix_spawn` is the supported one-call primitive that does fork+exec correctly, and it's
  available on both macOS and Linux.

### 2.2 The deadlock (the bug the plan warns about twice)

A pipe is a fixed-size kernel buffer, typically 64 KB. Writes block when it's full; reads block
when it's empty.

```
  shrinker                          child (./a.out)
     |  write 5 MB to stdin  ---->  [stdin pipe: FULL]     <- child isn't reading yet
     |  ...blocked...               |
     |                              |  printf lots of output
     |                              v
     |                          [stdout pipe: FULL]        <- shrinker isn't reading
     |                              |  ...blocked...
     v
   both blocked forever
```

The child fills its stdout buffer and blocks trying to write more. You fill its stdin buffer and
block trying to write more. Neither can proceed. Nothing times out, nothing crashes; your tool
just sits there.

The fix is to never block on any single fd. Set all three pipe fds non-blocking and drive them
from one `poll()` loop:

```
deadline = now + time_limit
while (any fd still open):
    timeout_ms = deadline - now
    poll({stdin_w: POLLOUT, stdout_r: POLLIN, stderr_r: POLLIN}, timeout_ms)
    if poll returned 0:            -> timed out: kill(-pid, SIGKILL); status = Timeout; break
    if stdin_w  is writable:       -> write next chunk; if all written, close(stdin_w)
    if stdout_r is readable:       -> read into buffer; 0 bytes means EOF, close it
    if stderr_r is readable:       -> same
wait4(pid, &status, 0, &rusage)
```

Note `close(stdin_w)` when you've written everything: that's what sends EOF to the child. A
competitive-programming solution reading with `while (cin >> x)` will loop forever if you never
close its stdin.

**Test this on day one.** `tests/unit/runner_test.cpp` should pipe 10 MB through `/bin/cat` and
assert it comes back identical. 10 MB is far past 64 KB, so a naive write-then-read implementation
hangs and the test times out. That single test is the difference between "works on my 20-byte
example" and "works".

### 2.3 SIGPIPE will kill *your* process

If you write to a pipe whose read end is closed (child already exited — crashed, or just
`return 0` before reading all input), the kernel sends `SIGPIPE`. The default action for SIGPIPE
is **terminate the process** — and the process being terminated is the shrinker, not the child.

Symptom: your tool silently vanishes mid-run with no error message, usually on the exact
candidate where the solution crashes early. Which, in a crash-hunting tool, is often.

Fix, once, in `main()`:

```cpp
::signal(SIGPIPE, SIG_IGN);   // now write() returns -1/EPIPE instead, which you can handle
```

### 2.4 Process groups, or: SIGKILL isn't enough

`kill(pid, SIGKILL)` kills the child. It does not kill anything the child forked. A solution that
spawns a helper leaves an orphan burning CPU, which then skews every subsequent timing
measurement and eventually fills your process table.

Put each child in its own process group at spawn time, then signal the whole group:

```cpp
posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
posix_spawnattr_setpgroup(&attr, 0);      // 0 = "new group, leader = the child"
// on timeout:
::kill(-pid, SIGKILL);                    // negative pid = "the whole process group"
```

### 2.5 Zombies

Every child that exits stays in the process table as a "zombie" until the parent calls
`wait`/`waitpid`/`wait4`. Spawn 50,000 children without reaping and you run out of PIDs. Always
reap, on every path — including the timeout path, where you must reap *after* the SIGKILL.

Use `wait4` rather than `waitpid`: same thing, but it also fills in a `struct rusage` giving you
the child's CPU time (`ru_utime + ru_stime`) in the same call.

### 2.6 Why CPU time beats wall-clock time for `--mode tle`

Wall-clock time measures how long the child took *including* time it spent descheduled while your
other 7 workers hogged the cores. Run the same candidate at `-j 1` and `-j 8` and you get
different wall times, so `-j 8` invents TLEs that don't exist and the shrinker chases ghosts.

CPU time is what the process actually consumed and is stable under load. Use CPU time for the
"is this a TLE?" decision, and keep wall clock only as a hard kill backstop so a genuinely
infinite loop still gets terminated.

### 2.7 Decoding exit status

```cpp
if (WIFEXITED(st))        exit_code   = WEXITSTATUS(st);   // normal return / exit()
else if (WIFSIGNALED(st)) term_signal = WTERMSIG(st);      // SIGSEGV=11, SIGABRT=6, SIGKILL=9
```

`SIGSEGV` is the classic out-of-bounds crash. `SIGABRT` usually means an assertion or a
`std::bad_alloc`/uncaught exception. `SIGKILL` on a child you killed means your timeout fired.
Note: a program that returns `1` from `main` is *not* a crash — competitive judges treat non-zero
exit as RE, so `--mode crash` should count both.

---

## 3. Why the "derived length" trick matters more than it looks

The obvious design stores every field in the tree, including `n`, and every deletion pass must
remember to decrement `n`. That works until you write the eighth pass and forget. Then the tool
emits a malformed input, the solution reads past end-of-input, produces garbage, differs from
brute, and the shrinker records "still interesting!" — and now it's optimising a bug that doesn't
exist. You'd spend an hour debugging *the debugger*.

The plan instead **doesn't store `n` at all**. It's computed at serialize time as
`array.kids.size()`. There's no invariant, so it can't be violated. This is a general principle
worth internalising: *make illegal states unrepresentable*, rather than writing code that
carefully maintains legality. Whenever you catch yourself writing "remember to also update X",
ask whether X can be derived instead.

Same reasoning drives the `Invalid` verdict being treated as a **fatal error** rather than
something to skip past. Since well-formedness is structural, an `Invalid` can only mean your
format spec doesn't describe this file. Silently continuing would produce confident nonsense; the
plan says abort with an explanation instead.

---

## 4. ddmin, worked by hand

"Delta debugging minimisation." The goal is a **1-minimal** input: one where removing any single
element makes the failure go away. (Not *globally* minimal — that's exponential. 1-minimal is the
standard, achievable guarantee.)

Say `a = [3, 1, 4, 1, 5, 9, 2, 6]` and the failure needs `1` and `9` present.

```
g = 2  -> chunks [3 1 4 1] [5 9 2 6]
         remove chunk 1 -> [5 9 2 6]      still fails?  no (no 1)
         remove chunk 2 -> [3 1 4 1]      still fails?  no (no 9)
g = 4  -> chunks [3 1] [4 1] [5 9] [2 6]
         remove [3 1] -> [4 1 5 9 2 6]    fails!  keep it, g = max(4-1,2) = 3, restart
g = 3  -> [4 1] [5 9] [2 6]
         remove [4 1] -> [5 9 2 6]        no
         remove [5 9] -> [4 1 2 6]        no
         remove [2 6] -> [4 1 5 9]        fails!  keep, g = 2, restart
g = 2  -> [4 1] [5 9]
         remove [4 1] -> [5 9]            no  (needs the 1)
         remove [5 9] -> [4 1]            no
g = 4  -> singletons: remove 4 -> [1 5 9] fails!  keep, g = 2
...converges to [1 9]
```

Key intuition: **start with big chunks**. If half the input is irrelevant, one test removes it.
Only when big removals stop working do you narrow down to individual elements. That's why it's
`O(n log n)`-ish in practice rather than `O(n²)`.

Why `g = max(g-1, 2)` after a success? Because removing a chunk left `g-1` chunks' worth of data,
and you want to keep testing at roughly the same granularity rather than restarting coarse.

---

## 5. Why binary search values instead of setting them to zero

Naive "shrink toward 0" destroys entire classes of bug. Consider an overflow bug that fires when
`a[i] * a[j] > 2^63`. Zeroing every value makes the product 0, no overflow, no repro. The reducer
concludes "not interesting" and keeps the big random values it started with — you learn nothing.

Binary search per value between the field's declared minimum and its current value converges on
*the threshold*, and the threshold is the diagnosis. Output like

```
a = [2147483647, 1]
```

tells you "this is a 32-bit overflow" without you reading a line of your own code. Output like
`a = [1836291744, 12]` tells you nothing.

Same reason `Field::lo` matters: if the problem guarantees `1 ≤ a[i]`, shrinking to `0` produces a
"failing" case that could never appear on the judge. A reducer that reports impossible inputs
gets ignored within a day.

---

## 6. Why `AlignValues` and `SortArray` are in the pass list

Deletion and value-shrinking only ever make things *smaller*. Some bugs need *structure*:

- A wrong tie-break only fires when two elements are **equal**. Shrinking values independently
  will basically never produce a duplicate by accident. `AlignValues` explicitly tries
  `a[i] := a[j]`, which finds it in a handful of tests.
- `SortArray` isn't about size at all — it's about *legibility*. If the input still fails after
  sorting, the bug doesn't depend on order, and `a = [1, 1, 2]` reads far better than
  `a = [2, 1, 1]`. Half the value of this tool is that the output explains itself.
- `Normalize` (all elements equal to one constant) is the same idea at maximum strength: it turns
  "random noise that happens to fail" into "any 3 equal values fail", which is a much stronger
  statement about your bug.

---

## 7. Determinism, and why it's a design constraint not a nicety

A tempting parallel implementation is: fire off all `g` candidate removals, take whichever
finishes first. Don't. Machine load varies, so "first to finish" varies, so the shrinker returns a
*different minimal input every run*. You'd then be unable to tell whether a change you made to
your solution fixed the bug or just perturbed the reducer.

Instead: fire them all off, but always accept the **lowest-index** success and kill the rest. You
get the parallel speedup and `-j 8` produces byte-identical output to `-j 1`. Make that an e2e
test — it's cheap and it protects a property users care about a lot.

Corollary: seed every RNG from `--seed` and print the seed in the report.

---

## 8. Threads: the two things that actually bite

1. **The cache is shared mutable state.** `std::unordered_map` is not thread-safe; two concurrent
   `insert`s can corrupt it. Guard it with a `std::mutex`. Don't reach for a lock-free map — at
   thousands of lookups per second, a mutex is free and correct.
2. **Cancellation must be cooperative.** When a lower-index candidate wins, in-flight workers need
   to stop. Use a `std::atomic<bool>` that workers check, plus SIGKILL on the children they
   spawned so you're not waiting 5 seconds for a doomed run to time out.

Run TSan. Data races are the one bug class where "it works on my machine, 100 times in a row" is
worth nothing.

---

## 9. Testing: what goes in which layer

| Layer | Speed | Use it for |
|---|---|---|
| Unit | ms | Anything pure: parsing, serializing, tree edits, ddmin against a fake predicate |
| Runner tests | ~seconds | The four subprocess footguns. `cat` with 10 MB, `sleep 10` + timeout, a segfaulting child, a forking child |
| Property tests | seconds | Invariants over *random* inputs, rather than examples you thought of |
| E2E | ~10s | The four demo bugs, end to end, asserting the actual minimal output |

**Property tests are unusually well suited to this project.** You don't need real binaries; you
need a random input and a random synthetic predicate (e.g. "interesting iff the array contains a
value > 500 and has length ≥ 2"). Then assert three things about whatever the shrinker returns:

1. it's still Interesting,
2. it's 1-minimal (removing any single element makes it Boring),
3. it re-parses under the same spec.

Run that over a few thousand random cases and you've covered far more ground than any set of
hand-written examples. This is the technique that will find your real bugs.

Also: write the test *before* the fix once you have a failing case. It's the only way to know the
fix worked and stays working.

---

## 10. Docker — what it's actually for here

Three concrete reasons, none of them "because everyone uses Docker":

1. **Linux coverage from a Mac.** Your `poll()`/`posix_spawn` code will behave slightly
   differently on Linux (pipe buffer sizes, `SIGCHLD` details). A `docker build` catches that
   before a user does.
2. **A README that works.** `docker run --rm shrink make demo` reproduces all four demo bugs on
   anyone's machine with no toolchain setup. That's the difference between a project people try
   and one they don't.
3. **Reproducible CI.** The same image locally and in GitHub Actions means "works in CI, fails
   locally" stops happening.

Keep it simple — a multi-stage Dockerfile (build stage with clang + cmake, runtime stage copying
just the binary and demos) and a `.dockerignore` excluding `build/`. Don't containerise your
day-to-day dev loop; building natively on the Mac is much faster and you want that iteration
speed. Docker is for verification and distribution.

---

## 11. Small C++ habits that pay off in this codebase

- **Value semantics for `Node`.** Candidates get copied constantly. A tree of
  `std::vector<Node>` copies in one call and has no ownership questions. Resist the urge to use
  `unique_ptr`/raw pointers "for performance" — inputs are kilobytes, and the process spawn costs
  10,000× more than the copy. Optimise the spawn count (that's what the cache is for), not the copy.
- **`enum class`, not `enum`.** `Verdict::Boring` can't be silently compared to an int.
- **`std::string_view` / `std::span` for parameters you only read**, `std::string`/`vector` for
  things you own. Never return a `string_view` into a temporary.
- **`[[nodiscard]]` on anything returning a `Verdict` or a `RunResult`.** Ignoring one of those is
  always a bug.
- **No exceptions across the subprocess boundary.** `run()` returns a `RunResult` with a status
  field; a failed spawn is a value, not a throw. Errors that are *expected* (child crashed, timed
  out) are data. Reserve exceptions/`std::abort` for programmer errors (spec invariant violated).
- **`-Wconversion` on.** Off-by-one and signedness bugs in index arithmetic are exactly the bug
  class this project is full of, and the compiler finds them for free.
- **`const` by default**, and prefer free functions over methods when there's no state.

---

## 12. Glossary

| Term | Meaning |
|---|---|
| **Predicate / oracle** | The black-box function "is this candidate still interesting?" Everything hangs off it |
| **Interesting** | Reproduces the bug we're chasing (not merely *some* bug — see signature locking) |
| **1-minimal** | Removing any single element stops it being interesting |
| **ddmin** | Delta debugging minimisation; the chunk-halving algorithm in §4 |
| **Fixpoint** | Repeat all passes until a full round changes nothing |
| **Slippage** | The reducer drifting onto a different bug than the one you started with |
| **Speculative execution** | Evaluating candidates you may not need, in parallel, and discarding losers |
| **Zombie** | An exited child not yet `wait`ed for |
| **Signature locking** | Requiring candidates to fail the *same way* as the original |

---

## 13. Things that will probably go wrong first

In rough order of likelihood:

1. Deadlock on a large input, because the poll loop only handles stdout. → §2.2.
2. The tool dies silently on a crashing solution. → SIGPIPE, §2.3.
3. Format spec parses but doesn't match the file, and you get confident nonsense. → make the
   input parser require exact consumption; leftover tokens are a hard error.
4. The shrinker "succeeds" but the minimal input reproduces a *different* bug. → signature
   locking, and always print both binaries' outputs for the final candidate so you can eyeball it.
5. Parallel mode gives different answers each run. → index-ordered selection, §7.
6. A pass forgets to update something and emits a malformed candidate. → derived fields, §3.
7. You spend three hours on a pretty progress bar before the algorithm works. → it's M7 for a reason.
