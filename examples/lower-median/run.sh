#!/bin/sh
set -eu
example_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$example_directory/../find-shrink.sh"
exec "$shrink_executable" \
    --solution-timeout 10s --reference-timeout 10s \
    --input "$example_directory/original.txt" \
    --solution "$example_directory/solution" --reference "$example_directory/reference" \
    --min-length 1 --max-length 8 --min-value=-100 --max-value 100 \
    --sorted ascending --output "${1:-$example_directory/reduced.txt}"
