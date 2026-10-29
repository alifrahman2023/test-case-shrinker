#!/bin/sh
set -eu
if [ "$(uname -s):$(uname -m)" != "Darwin:arm64" ]; then
    echo "These example binaries target macOS on Apple Silicon (ARM64)." >&2
    exit 2
fi
examples_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for example in duplicates width maximum sum-last lower-median; do
    for role in solution reference; do
        echo "Building $example/$role"
        xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Wpedantic \
            -arch arm64 -mmacosx-version-min=15.0 \
            "$examples_directory/$example/$role.cpp" \
            -o "$examples_directory/$example/$role"
    done
done
