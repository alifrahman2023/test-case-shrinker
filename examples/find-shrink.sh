# Sourced by each example's run.sh after it sets example_directory.
if [ -n "${SHRINK_BIN:-}" ]; then
    shrink_executable=$SHRINK_BIN
elif command -v shrink >/dev/null 2>&1; then
    shrink_executable=$(command -v shrink)
else
    repository_directory=$(dirname "$(dirname "$example_directory")")
    if [ -x "$repository_directory/build/release/shrink" ]; then
        shrink_executable=$repository_directory/build/release/shrink
    elif [ -x "$repository_directory/build/debug/shrink" ]; then
        shrink_executable=$repository_directory/build/debug/shrink
    else
        echo "Build/install shrink first, or set SHRINK_BIN to its executable path." >&2
        exit 2
    fi
fi
if [ ! -x "$example_directory/solution" ] || [ ! -x "$example_directory/reference" ]; then
    echo "Build the example programs with examples/build.sh first." >&2
    exit 2
fi
