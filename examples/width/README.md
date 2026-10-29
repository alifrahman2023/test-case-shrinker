# Multiply two values

Given exactly two nonnegative integers, print their mathematical product.

**Input:** the length `2`, followed by two integers. **Output:** one integer.

**Constraints:** `n = 2`, `0 <= a[i] <= 100000`. Duplicates and either order
are allowed. The largest permitted product is 10000000000.

The solution multiplies in an unsigned 32-bit integer, so its answer wraps modulo
4294967296. The reference uses an unsigned 64-bit integer. The faulty arithmetic
is defined; it does not depend on signed overflow.

The supplied `[100000, 100000]` produces **1410065408** from the solution and
**10000000000** from the reference. A typical reduced input is `[65536, 65536]`,
with answers **0** and **4294967296**.

```sh
./solution < original.txt
./reference < original.txt
./run.sh
cat reduced.txt
```

The binaries target macOS 15+ ARM64. Rebuild with `../build.sh`.
`run.sh` finds installed `shrink` or the repository's Release/Debug build.
Set `SHRINK_BIN=/absolute/path/to/shrink` to select another executable.
An optional first argument chooses the output file: `./run.sh /tmp/product.txt`.
