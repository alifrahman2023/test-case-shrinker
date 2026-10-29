#!/bin/sh
set -eu
example_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$example_directory/../find-shrink.sh"
exec "$shrink_executable" \
    --solution-timeout 10s --reference-timeout 10s \
    --input "$example_directory/original.txt" \
    --solution "$example_directory/solution" --reference "$example_directory/reference" \
    --min-length 2 --max-length 2 --min-value 0 --max-value 100000 \
    --output "${1:-$example_directory/reduced.txt}"
