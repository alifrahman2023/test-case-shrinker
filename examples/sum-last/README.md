# Sum an array

Given an integer array, print the sum of its elements. The empty array sums to zero.

**Input:** a length `n`, followed by exactly `n` integers. **Output:** one integer.

**Constraints:** `0 <= n <= 8`, `-100 <= a[i] <= 100`. The array may be
unsorted and contain duplicates. All permitted sums fit in a signed 64-bit integer.

The solution omits the last element when summing. The reference includes every
element.

The supplied `[5, 0, 8, -3]` produces **13** from the solution and **10** from
the reference. A typical reduced input is `[1]`, with answers **0** and **1**.

```sh
./solution < original.txt
./reference < original.txt
./run.sh
cat reduced.txt
```

The binaries target macOS 15+ ARM64. Rebuild with `../build.sh`.
`run.sh` finds installed `shrink` or the repository's Release/Debug build.
Set `SHRINK_BIN=/absolute/path/to/shrink` to select another executable.
An optional first argument chooses the output file: `./run.sh /tmp/sum.txt`.
