# Source release verification

Release publication and tag creation require the author's approval. Neither the
CI workflow nor the packaging scripts create tags, publish releases, update a
tap remotely, close the parent spec, or modify the decision map. Until the first
approved tag exists, the formula and tagged-install commands are staged release
infrastructure. Passing PR checks do not mean a tagged release has been verified.

## Candidate and exact-release checks

Every PR and main update runs [Native Mac checks](.github/workflows/ci.yml) on
the explicit **macOS 15 ARM64** baseline and **macOS 26 ARM64** compatibility
runner. Both jobs extract an archive of the tested commit, rather than using
untracked working-tree files. Each runs the complete native Debug CTest suite,
including a CLI-only optimized Release installation, installed help/version,
and both demonstrations outside the build directory. The installation check
asserts that only `bin/shrink` is installed and no Catch2 download is needed.

Each job also source-builds through a temporary personal Homebrew tap, runs
`brew test`, and repeats installed-command checks and both demonstrations. For
PR/main checks, only the formula's source stanza is replaced with the same
candidate archive's file URL and SHA-256. The formula's build and test logic are
unchanged. The actual checked formula is retained as evidence. This candidate
override is never committed or presented as a tagged-release verification.

After the author approves and creates a tag, run the workflow manually with
that existing tag. For example, after approval of `v0.1.0`:

```sh
gh workflow run ci.yml --ref main -f tag=v0.1.0
gh run list --workflow ci.yml --event workflow_dispatch
# Use the run ID printed above:
gh run watch RUN_ID --exit-status
gh run download RUN_ID --dir build/release-evidence
```

The workflow checks out the tag, checks that the formula names it, records its
commit, downloads the tag archive used in the manual instructions, and compares
its complete extracted contents with `git archive` of the tagged commit. It runs
the full checks from that source and installs Homebrew using the actual stable
tagged-source formula, without the candidate override. Keep approved tags
immutable; never retarget one to repair a failed release. Correct failures and
obtain approval for a new version instead. Workflow dispatch must run from a
branch containing this workflow; the selected tag must contain this verification
infrastructure too.

Retain both jobs' logs and evidence artifacts, with tag, commit, archive SHA-256,
actual macOS, compiler, CMake, Homebrew and pinned dependency versions. Record the
run URL with the eventual release. Do not publish if either job fails. A passing
development checkout, an old version's run, or a formula-only smoke test cannot
replace these exact-source checks. No byte-identical-binary guarantee is made.

## Contract review before approval

The release suite reuses the tests implemented with each feature; release time
is not a deferred behavioral-testing phase. Review the approved
[spec #10](https://github.com/alifrahman2023/test-case-shrinker/issues/10) and
[installation issue #22](https://github.com/alifrahman2023/test-case-shrinker/issues/22)
against these existing checks:

| Contract | Evidence in the full suite |
| --- | --- |
| All required and optional CLI options, both value forms, arguments, invalid configurations | `tests/cli.test.cpp`, `cli_information` |
| Exit classes 0/1/2/3/4/130 and operational-error precedence | CLI, cancellation, execution-event and reduction tests |
| Strict input grammar, bounds, sortedness, uniqueness, exact token comparison | Input, CLI and real runner tests |
| Accepted inputs strictly smaller, valid and failure-preserving; independent local checks; repeatability | Reduction and CLI value/removal tests; both demonstrations |
| Exact cache identity under collisions, completed on/off equivalence, eviction, final-round uncertainty | Reduction/CLI cache tests |
| Evaluation and sleep-inclusive time budgets, unfinished work discarded | Reduction/CLI time-limit and execution-event tests |
| Bounded evidence, quiet mode, retained baseline/final evidence | CLI baseline/progress tests and demonstrations |
| Protected files, checkpoint/final saves, recovery and fresh resume | CLI, saving and cancellation tests; demonstrations |
| Concurrent real execution, output caps, status/error distinction, direct-child cleanup | Real runner tests |
| Exceptional cleanup, retained ownership, recovery and destructor/lifetime contract | Scripted execution-event tests |
| Owning core results, callback exceptions and save/notification hooks | Reduction lifecycle tests |

The [README](README.md) states the exact local guarantee and repeatability
exclusions, supplied-program prerequisites, sleep-inclusive limits and cleanup
bounds, callback/runner lifetime rules, last-successful-checkpoint recovery and
lack of fsync/power-loss durability. The [installation instructions](INSTALL.md)
and [demonstrations](examples/README.md) must agree with it. Crash reduction,
Linux, Intel Macs, and descendant support remain deferred.

## Dependency and runner maintenance

Catch2 is pinned to v3.8.1 and fetched only in test builds. Review dependency and
GitHub Action updates explicitly, run the full suite before adopting them, and
record the actual compiler/CMake versions printed by configuration. Named hosted
runners still receive image/tool updates.

Before every release, consult the authoritative
[runner image list](https://github.com/actions/runner-images#available-images)
and [runner maintenance policy](https://github.com/actions/runner-images#software-and-image-support).
At implementation time, `macos-26` is the newest generally available stable macOS
runner; [GitHub's GA announcement](https://github.blog/changelog/2026-02-26-macos-26-is-now-generally-available-for-github-hosted-runners/)
confirms ARM64 availability. Update the compatibility matrix when a newer stable
runner becomes available; do not silently replace the named supported baseline
with a moving `macos-latest` label. If the two become the same, run it once.

Review retirement notices before every release and during routine dependency
maintenance. Before baseline retirement, add and validate its proposed successor
in parallel, check source Homebrew/manual installs, then obtain an explicit
support-baseline decision and update documentation and required checks. Keep
macOS 15 coverage until that migration is agreed. Configure the baseline's
`macOS 15 ARM64 Debug and installed Release` check as required in repository
rules; do not weaken it to bypass an installation failure.
