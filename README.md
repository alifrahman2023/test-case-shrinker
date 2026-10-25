# shrink

A format-aware test-case reducer for competitive programming.

Give it a solution binary, a reference (brute-force) binary, and a failing input, and it hands
back the smallest input that still reproduces **the same** failure.

```
$ shrink --sol ./a.out --brute ./brute --format "n; a[n]" fail.txt
[############] 847 candidates · 61 kept · 312 cache hits
original: n=2000, 11.4 KB
minimal:  n=3, a=[1, 1, 2]
  sol:   4
  brute: 3
```

## Why a format spec

Generic reducers fail on competitive-programming inputs because deleting a value desynchronises
the length header, and the resulting malformed input still "fails" — just for a different reason.
There are only about six input shapes in the whole domain, so a 20-character format spec buys
full grammar-awareness. Every candidate is well-formed by construction: a length field like `n`
is never stored in the tree at all, it is emitted at serialize time as the array's size.

## Status

Early development. The milestone map lives in [plans/plan.md](plans/plan.md), with the reasoning
behind each decision in [context/notes.md](context/notes.md).

| Milestone | State |
|---|---|
| M0 · build skeleton, `shrink --version`, Catch2, CI | done |
| M1 · process runner | done |
| M2 · format spec + IR | done |
| M3 · oracle + cache | not started |
| M4 · delete/shrink passes + driver | not started |
| M5 · relabel, normalize, align, sort, grid passes | not started |
| M6 · thread pool, deterministic parallel rounds | not started |
| M7 · CLI polish, progress, JSON report | not started |
| M8 · demos, Docker, docs | not started |

`shrink` can parse a format spec and an input file and print the resulting tree. Reduction
itself is not implemented yet.

```sh
shrink --preset graph --dry-run fail.txt
```

Presets are `array`, `multi`, `graph`, `grid`, `string` and `scalars`; `--format` takes the DSL
directly, for example `--format "n; a[n]"`.

## Build

Requires C++20, CMake 3.24 or newer, and Ninja. Apple clang 17 or GCC 13 and up are known to
work. CLI11 and Catch2 are fetched by the build; there is nothing to install first.

```sh
cmake --preset dev        # Debug + AddressSanitizer + UndefinedBehaviorSanitizer
cmake --build --preset dev
ctest --preset dev
```

Other presets: `release` for an optimized build, `tsan` for ThreadSanitizer, `ci` for what the
GitHub Actions workflow runs.

## Checking the Linux build before you push

CI builds with GCC on Linux, and GCC warns about things clang does not. A clean build on a Mac
is therefore not proof that CI will pass. Run this once after cloning:

```sh
scripts/bootstrap.sh
```

From then on every `git push` first runs the Linux CI jobs in Docker against the commit being
pushed: the clang-format check, then the `ci`, `dev` and `tsan` presets with GCC 13. A failure
aborts the push. `git push --no-verify` skips it once.

To run the same checks by hand against uncommitted work:

```sh
scripts/check-linux.sh          # all three presets
scripts/check-linux.sh ci       # just one
```

Docker Desktop is started automatically if it is not already running. Builds are cached in a
Docker volume, so only the first run is slow.

## Layout

| Path | Holds |
|---|---|
| `include/shrink/` | public headers for the core library |
| `src/` | core implementation, plus a thin `main.cpp` |
| `tests/unit/` | Catch2 tests linked against `shrink_core` |
| `tests/helpers/` | a stand-in child process the runner tests drive |
| `docker/`, `scripts/`, `.githooks/` | the Linux CI check and the pre-push hook that runs it |
| `plans/`, `context/` | the design plan and the reasoning behind it |

Everything of substance lives in the `shrink_core` static library so tests link it directly.
`main.cpp` stays a shell around argument parsing and reporting.

## License

MIT. See [LICENSE](LICENSE).
