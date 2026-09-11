# `shrink` — format-aware test-case reducer

## 1. Goal

Given a solution binary, a reference (brute-force) binary, and a failing input, produce the
smallest input that still reproduces **the same** failure.

```
$ shrink --sol ./a.out --brute ./brute --format "n; a[n]" fail.txt
[############] 847 candidates · 61 kept · 312 cache hits
original: n=2000, 11.4 KB
minimal:  n=3, a=[1, 1, 2]
  sol:   4
  brute: 3
```

Core bet: generic reducers fail on competitive-programming inputs because deleting a value
desynchronises the length header, and the resulting malformed input still "fails" — just for a
different reason. There are ~6 input shapes in the whole domain, so a 20-character format spec
buys full grammar-awareness. Every candidate we generate is well-formed **by construction**.

## 2. Scope

**In:** POSIX (macOS + Linux), stdin/stdout binaries, single-file text inputs, the six shapes in §4.
**Out:** Windows (Docker covers it), interactive problems, file-based I/O, multi-file inputs,
binary input formats, automatic format inference (v2 maybe).

## 3. Architecture

Single static library `shrink_core` + thin `main.cpp`, so tests link the core directly.

| Module | Header | Responsibility |
|---|---|---|
| `spec` | `spec.hpp` | Parse the format DSL → `Spec` (field table + structural template + layout) |
| `ir` | `ir.hpp` | `Node` tree, parse input → tree, serialize tree → text, structural edits |
| `runner` | `runner.hpp` | `posix_spawn`, pipes, `poll()` I/O pump, timeout, exit-status decode |
| `oracle` | `oracle.hpp` | Run sol+brute, compare, classify → `Verdict`; failure-signature locking |
| `cache` | `cache.hpp` | Hash serialized candidate → memoized `Verdict` (thread-safe) |
| `pool` | `pool.hpp` | Fixed thread pool, index-ordered first-success, cooperative cancel |
| `passes` | `passes.hpp` | ddmin, value binary search, relabel, normalize, align, sort, grid |
| `driver` | `driver.hpp` | Fixpoint loop over passes, budgets, progress events, final 1-minimality proof |
| `cli` | `main.cpp` | Arg parsing (CLI11), progress rendering, report output, exit codes |

Dependency direction is strictly downward: `cli → driver → passes → oracle → {cache, runner, pool} → ir → spec`.
Nothing below `oracle` knows what a subprocess is; that is what makes passes unit-testable
against a fake predicate.

## 4. Format spec DSL

```
spec    := stmt (';' stmt)*
stmt    := scalar | array | grid | repeat
scalar  := IDENT (':' type)? ('in' range)?
array   := IDENT '[' size ']' (':' type)? ('in' range)?
         | '{' IDENT (',' IDENT)* '}' '[' size ']' ('in' range)?     # tuples / edges
grid    := IDENT '[' size ']' '[' size ']' ':' 'char'
repeat  := IDENT '{' spec '}'                                        # t { n; a[n] }
size    := IDENT | INT
range   := NUM '..' NUM        # NUM may be an int literal or a previously-declared IDENT
type    := 'int' | 'str' | 'char'                                    # default int
```

An `IDENT` used as a *size* is rewritten to a **derived** field (§5). `in lo..hi` is a real
constraint: shrinking never leaves the declared range, and if `hi` names a field, that field
becomes a **label domain** (§7 relabel).

Built-in presets (`--preset <name>`), all expressible in the DSL above:

| Preset | Spec |
|---|---|
| `array` | `n; a[n]` |
| `multi` | `t { n; a[n] }` |
| `graph` | `n; m; {u,v}[m] in 1..n` |
| `grid` | `r; c; g[r][c]:char` |
| `string` | `n; s[n]:char` |
| `scalars` | `a; b; c` |

Layout on serialize: one statement per line; arrays on one line space-separated; `char` arrays
joined with no separator; each grid row its own line; `repeat` emits its body per iteration.
`--layout preserve` (v2) would re-use the original file's line breaks.

## 5. IR — validity by construction

```cpp
struct Node {                      // value semantics: copying a candidate is a deep copy
  enum class Tag { Seq, Repeat, Array, Record, Int, Str };
  Tag tag;
  int64_t           ival{};        // Tag::Int
  std::string       sval;          // Tag::Str
  std::vector<Node> kids;          // Seq / Repeat / Array / Record
  int               field{-1};     // index into Spec::fields
};
```

```cpp
struct Field {
  std::string name;
  enum class Role { Free, Derived, Domain } role;
  enum class Type { Int, Str, Char } type;
  int     len_of{-1};              // Derived: which array/repeat it counts
  int64_t lo{INT64_MIN}, hi{INT64_MAX};
  int     hi_field{-1};            // 'in 1..n' -> label domain
};
```

**The key decision:** a `Derived` field is *not stored in the tree at all*. `n` is emitted at
serialize time as `kids.size()` of the array it measures. There is no length-field invariant to
maintain, so no pass can ever break it — as opposed to "remember to decrement `n` whenever you
delete", which is a bug waiting to happen in every pass. `Invalid` should therefore be
unreachable; if the oracle ever returns it we abort with *"your format spec does not match this
input"* rather than shrinking garbage.

Structural edits are free functions returning a new `Node`:
`erase_elems(node, path, indices)`, `set_int(node, path, v)`, `map_labels(node, field, fn)`.

## 6. Oracle & verdicts

```cpp
enum class Verdict { Interesting, Boring, Invalid };
```

| Mode | Interesting when |
|---|---|
| `diff` (default) | sol output ≠ brute output, or sol crashes |
| `crash` | sol exits non-zero or dies on a signal (brute not required) |
| `tle` | sol CPU time > `--tle-threshold` (brute not required) |
| `checker` | user checker `checker <input> <sol_out> <brute_out>` exits non-zero |

Rules that keep the search honest:

1. **Brute is ground truth.** If brute crashes or times out, the candidate is `Invalid`, not
   `Interesting`. That is the main line of defence against drifting outside the problem's
   constraints.
2. **Signature locking.** Record the original failure's signature (verdict kind + terminating
   signal + exit code, and in `diff` mode a coarse shape of the mismatch). Candidates must match
   the same signature or they are `Boring`. `--any-failure` relaxes this. Without it a reducer
   happily converges onto a *different, easier* bug.
3. **Comparison** is token-wise by default (`--cmp tokens`); `exact` and `float:<eps>` available.
4. **TLE uses CPU time** (`ru_utime + ru_stime` from `wait4`), not wall clock, so it stays stable
   under parallel load. Wall clock remains a hard SIGKILL backstop at `--tl`.

Pre-flight, before any shrinking: assert the *original* input is `Interesting`. If it is not,
exit 1 with a clear message — the most common user error by far.

## 7. Passes

Each pass is `std::vector<Node> candidates(const Spec&, const Node&)` plus an apply step; the
driver owns the predicate. All emitted candidates are well-formed.

| Pass | What it does | Why it exists |
|---|---|---|
| `DeleteElems` | ddmin over each `Array`'s elements | the workhorse |
| `DropIters` | ddmin over `Repeat` iterations | collapses `t` test cases to one |
| `ShrinkValues` | binary search each `Int` toward `Field::lo` | **not** toward 0 — see below |
| `Relabel` | compact a label domain to `1..k`, drop dangling refs, lower the domain field | without it graph shrinking is a no-op: every deletion breaks an edge |
| `Normalize` | try all elements of an array = one constant (`lo`, then modal value, then `a[0]`) | turns random noise into an obviously-structured counterexample |
| `AlignValues` | try `a[i] := a[j]` to reduce distinct values | tie-break bugs *need* duplicates; shrinking alone won't find them |
| `SortArray` | try replacing an array with its sorted copy | if it survives, order is irrelevant → far more readable output |
| `GridLines` | delete whole grid rows/columns (both dims derived) | 2-D deletion needs 2-D moves |

ddmin over one array (1-minimal on exit):

```
g = 2
while g <= |a|:
    for each of the g chunks c (in index order):
        if test(a \ c) == Interesting:
            a = a \ c;  g = max(g - 1, 2);  restart while-body
    g *= 2
```

**Never hardcode zero.** If the bug needs `a[i] > 2^31`, forcing values down to 0 destroys the
reproduction; per-value binary search stops at whichever threshold matters and the result
(`a = [2147483647, 1]`) *names the bug*. Binary search per value between `Field::lo` and the
current value, keeping the smallest still-interesting value.

Driver: run every pass in a fixed order, loop until a full round changes nothing (fixpoint).
Budgets: `--max-tests`, `--max-time`. Finish with an explicit 1-minimality pass so the claim in
the output is earned, not assumed.

## 8. Concurrency & determinism

- Fixed thread pool, `-j` defaults to `hardware_concurrency()`.
- The `g` chunk-removals inside one ddmin round are independent → fan out.
- **Take the lowest-index success, not the first to finish.** A debugging tool that returns a
  different answer on every run is worthless; index-ordered selection makes `-j 8` produce
  bit-identical output to `-j 1`. Once a lower index wins, SIGKILL the speculative children.
- Cache (`unordered_map<uint64_t, Verdict>` + mutex, FNV-1a or xxhash over the serialized bytes)
  is checked before dispatch and populated after. Expect 30–40% of tests to be hits.
- ThreadSanitizer build is a required CI job, not optional.

## 9. Process runner — the fiddly part

```cpp
struct RunResult {
  enum class Status { Ok, NonZeroExit, Signaled, Timeout, SpawnFailed } status;
  int exit_code{}, term_signal{};
  std::string out, err;
  std::chrono::microseconds wall{}, cpu{};
};
RunResult run(const std::vector<std::string>& argv, std::string_view stdin_data, Limits);
```

Non-negotiables (each one is a hang or a crash if skipped):

1. `signal(SIGPIPE, SIG_IGN)` once at startup — otherwise writing to a child that exited early
   kills *the shrinker*.
2. Single `poll()` loop over the child's stdin (write), stdout (read) and stderr (read)
   simultaneously. Writing stdin to completion before reading stdout deadlocks as soon as the
   child's stdout pipe buffer (64 KB) fills.
3. Close the parent's copies of the child's pipe ends immediately after spawn, or EOF never arrives.
4. `posix_spawnattr_setflags(POSIX_SPAWN_SETPGROUP)` + `setpgroup(0)`, then `kill(-pid, SIGKILL)`
   on timeout, so forked grandchildren die too.
5. Reap with `wait4` to get `rusage` (CPU time) in the same call; never leave zombies.
6. Cap captured stdout at `--max-output` (default 64 MB) so a runaway child can't OOM us.
7. `posix_spawn` directly, never `/bin/sh -c`, never temp files.

## 10. CLI

```
shrink [options] <input-file>

  --sol <cmd>            solution binary (required)
  --brute <cmd>          reference binary (required except --mode crash|tle)
  --checker <cmd>        custom checker for multi-answer problems
  --format <spec>        format DSL string
  --preset <name>        array | multi | graph | grid | string | scalars
  --mode <m>             diff (default) | crash | tle
  --tl <dur>             per-run wall limit, default 5s
  --tle-threshold <dur>  CPU time that counts as TLE, default 2s
  --cmp <m>              tokens (default) | exact | float
  --eps <f>              for --cmp float
  --any-failure          disable failure-signature locking
  -j <n>                 parallel workers, default = cores
  --max-tests <n>        budget, default 100000
  --max-time <dur>       wall budget for the whole run
  -o <file>              write the minimal input (default: stdout)
  --json <file>          machine-readable report
  --dry-run              parse spec + input, print the tree, run nothing
  -v / --quiet
```

Progress goes to **stderr**, the minimal input to **stdout**, so `shrink ... > min.txt` works.

Exit codes: `0` shrunk · `1` original input is not interesting · `2` spec/parse error ·
`3` binary missing or spawn failure · `4` internal invariant violated.

## 11. Repo layout

```
CMakeLists.txt  CMakePresets.json  .clang-format  .clang-tidy  .editorconfig  .gitignore
Dockerfile  .dockerignore  README.md
include/shrink/{spec,ir,runner,oracle,cache,pool,passes,driver}.hpp
src/{main,spec,ir,runner,oracle,cache,pool,driver}.cpp
src/passes/{ddmin,values,relabel,normalize,grid}.cpp
tests/unit/{spec,ir,ddmin,relabel,runner,cache}_test.cpp
tests/property/shrink_props.cpp
tests/e2e/{run_demo.cmake,expected/}
demos/{01-off-by-one,02-tie-break,03-overflow,04-greedy-shape}/{sol.cpp,brute.cpp,gen.cpp,spec.txt,fail.txt,README.md}
scripts/{bootstrap.sh,record_gif.sh}
```

## 12. Build & dependencies

- C++20, CMake ≥ 3.24, Apple clang 17 / GCC 13+.
- Targets: `shrink_core` (STATIC) → `shrink` (exe), `shrink_tests` (exe).
- FetchContent deps, pinned by git tag: **CLI11** (args), **Catch2 v3** (tests). Hash is a
  30-line FNV-1a in-tree — no dependency needed, and it's a useful thing to have written once.
- `CMakePresets.json`: `dev` (Debug + `-fsanitize=address,undefined`), `tsan`, `release`
  (`-O2 -DNDEBUG`), `ci`.
- Warnings as errors on the core: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`.
- `Dockerfile` (Ubuntu 24.04 + clang) for a reproducible Linux build, Linux CI coverage from a
  Mac, and a one-command demo for README readers. Nice-to-have, not on the critical path.

## 13. Testing strategy

| Layer | Tool | What it covers |
|---|---|---|
| Unit | Catch2 | spec parser accept/reject; input parse↔serialize round-trip; derived lengths stay correct after every structural edit; ddmin against a **fake in-process predicate** (`interesting ⟺ contains 1 and 7` → must yield exactly `[1,7]`); relabel; cache |
| Runner | Catch2 | `cat` echoing 10 MB back (the deadlock regression test); `sleep 10` killed at `--tl`; a segfaulting child reported as `Signaled`; a forking child fully reaped |
| Property | Catch2 GENERATE + a seeded RNG | for random inputs × random synthetic predicates: result is still Interesting, result is 1-minimal, result re-parses, result ⊆ original |
| E2E | CTest | each `demos/*` shrinks to its expected minimal form within a test budget |
| Sanitizers | ASan/UBSan + TSan presets | required CI jobs |

Every run takes `--seed`; with a fixed seed and index-ordered selection, runs are reproducible
regardless of `-j`.

## 14. Milestones

| # | ~Time | Deliverable | Acceptance |
|---|---|---|---|
| M0 | 0.5h | CMake skeleton, `shrink --version`, Catch2 wired, CI green | `ctest` runs 1 trivial test |
| M1 | 1.5h | `runner` | 10 MB through `cat` round-trips; `sleep 10` killed at 1s; segfault decoded; ASan clean |
| M2 | 1h | `spec` + `ir` parse/serialize | round-trip test passes for all 6 presets |
| M3 | 1h | `oracle` + `cache` + pre-flight check | on demo 1, correctly reports the original input interesting; brute-crash ⇒ Invalid |
| M4 | 2h | `DeleteElems` + `ShrinkValues` + fixpoint driver | demo 1 (`n; a[n]`, 2000 values) → 1-minimal, serial |
| M5 | 1h | `Relabel` + `Normalize` + `AlignValues` + `SortArray` + `GridLines` | demos 2 and 4 shrink to their expected forms |
| M6 | 1h | thread pool + index-ordered parallel rounds | `-j 8` output byte-identical to `-j 1`; TSan clean |
| M7 | 1h | CLI polish, progress bar, side-by-side report, `--json` | matches the §1 sample output |
| M8 | 1.5h | 4 demo bugs, README, GIF, Dockerfile | `docker run … make demo` reproduces all four |

Critical path is M1 → M4. M6 and M8 are droppable if time runs out; M0–M5 alone is already a
usable tool.

## 15. Risks

| Risk | Mitigation |
|---|---|
| Pipe deadlock eats the first day | Build `runner` first, in isolation, with the `cat` test as the gate (M1) |
| Reducer converges on a *different* bug | Failure-signature locking (§6.2), on by default |
| Reducer leaves the problem's constraints | `in lo..hi` in the spec + brute-crash ⇒ `Invalid` |
| Spec doesn't match the file, silent garbage | Parse must consume the input exactly; leftover or missing tokens = hard error, not a warning |
| Non-deterministic solutions (uninitialised memory, hash order) | Detect by running the original 3× at startup; warn loudly and suggest `--mode crash` |
| Parallel load causes false TLEs | CPU time, not wall clock, as the TLE metric (§6.4) |
| Value shrinking destroys overflow bugs | Binary search per value to `Field::lo`, never to 0 (§7) |
