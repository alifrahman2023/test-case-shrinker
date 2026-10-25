#!/usr/bin/env bash
# Run the Linux CI jobs inside Docker: the clang-format check, then configure, build and
# test each preset with the GCC the GitHub runner uses.
#
#   scripts/check-linux.sh                  working tree, every Linux preset CI runs
#   scripts/check-linux.sh ci               working tree, just the ci preset
#   scripts/check-linux.sh --rev HEAD       a commit instead of the working tree
#
# Builds persist in a Docker volume, so after the first run only changed files recompile.
# The container is native to this machine (arm64 on Apple silicon) while GitHub runs x86_64.
# Compiler warnings are the same on both; that difference rarely matters for anything else.
set -eo pipefail

repo=$(git rev-parse --show-toplevel)
cd "$repo"

image=shrink-linux-ci
volume=shrink-linux-ci-work
rev=""
presets=()

while [ $# -gt 0 ]; do
  case "$1" in
    --rev) rev=$2; shift 2 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) presets+=("$1"); shift ;;
  esac
done
# The Linux rows of .github/workflows/ci.yml.
[ ${#presets[@]} -eq 0 ] && presets=(ci dev tsan)

if ! command -v docker >/dev/null 2>&1; then
  echo "check-linux: docker is not installed" >&2
  exit 1
fi
if ! docker info >/dev/null 2>&1; then
  if [ "$(uname)" = "Darwin" ] && open -Ra Docker 2>/dev/null; then
    echo "check-linux: starting Docker Desktop..."
    open -a Docker
    for _ in $(seq 1 90); do
      docker info >/dev/null 2>&1 && break
      sleep 2
    done
  fi
  if ! docker info >/dev/null 2>&1; then
    echo "check-linux: Docker is installed but its engine is not running. Start Docker and retry." >&2
    exit 1
  fi
fi

if ! build_log=$(docker build -q -t "$image" -f docker/linux-ci.Dockerfile docker 2>&1); then
  echo "$build_log" >&2
  echo "check-linux: could not build the $image image" >&2
  exit 1
fi

# Ship the source in as a tar stream rather than a bind mount. That way --rev checks exactly
# what a commit contains, and nothing the container does can touch the Mac's build/ dirs.
if [ -n "$rev" ]; then
  sha=$(git rev-parse --verify "$rev^{commit}")
  label="commit ${sha:0:10}"
  make_tar() { git archive --format=tar "$sha"; }
else
  label="working tree"
  make_tar() {
    git ls-files -z --cached --others --exclude-standard |
      while IFS= read -r -d '' f; do
        if [ -e "$f" ]; then printf '%s\0' "$f"; fi  # skip tracked files deleted locally
      done |
      COPYFILE_DISABLE=1 tar --null -T - -cf -
  }
fi

echo "check-linux: $label, presets: ${presets[*]}"

inner=$(cat <<'IN_CONTAINER'
set -eo pipefail
exec 9>/work/.lock && flock 9  # one run at a time per volume

rm -rf /tmp/in && mkdir -p /tmp/in /work/src /work/build
tar --warning=no-unknown-keyword -x -C /tmp/in
# Compare contents, not timestamps, so unchanged files keep their old mtimes and Ninja only
# rebuilds what actually changed.
rsync -rc --delete /tmp/in/ /work/src/
cd /work/src

passed=()
failed=()

echo
echo "== clang-format $(clang-format --version | grep -o '[0-9][0-9.]*' | head -1) =="
if find include src tests \( -name '*.cpp' -o -name '*.hpp' \) -print0 |
     xargs -0 clang-format --dry-run --Werror; then
  passed+=(format)
else
  failed+=(format)
fi

for p in $PRESETS; do
  echo
  echo "== $p =="
  dir=/work/build/$p
  if ! cmake --preset "$p" -B "$dir" >"/tmp/configure-$p.log" 2>&1; then
    tail -30 "/tmp/configure-$p.log"
    failed+=("$p")
    continue
  fi
  if cmake --build "$dir" -- --quiet && ctest --test-dir "$dir" --output-on-failure --no-tests=error; then
    passed+=("$p")
  else
    failed+=("$p")
  fi
done

echo
echo "passed: ${passed[*]:-none}"
echo "failed: ${failed[*]:-none}"
[ ${#failed[@]} -eq 0 ]
IN_CONTAINER
)

start=$(date +%s)
if make_tar | docker run --rm -i -v "$volume":/work -e PRESETS="${presets[*]}" "$image" bash -c "$inner"; then
  echo "check-linux: passed in $(( $(date +%s) - start ))s"
else
  echo "check-linux: FAILED after $(( $(date +%s) - start ))s" >&2
  exit 1
fi
