# Find the lower median

Given a nonempty array sorted in nondecreasing order, print the element at
zero-based index `floor((n - 1) / 2)`. For even lengths, this is the smaller of
the two middle elements.

**Input:** a length `n`, followed by exactly `n` integers. **Output:** one integer.

**Constraints:** `1 <= n <= 8`, `-100 <= a[i] <= 100`, sorted ascending
(equal neighbors are allowed).

The solution uses index `n / 2`, returning the upper median on even-length arrays.
The reference uses the lower-median index.

The supplied `[-9, -4, 1, 3, 8, 20]` produces **3** from the solution and **1**
from the reference. A typical reduced input is `[0, 0, 0, 1, 1, 1]`, with answers
**1** and **0**. Removing any single element makes the length odd and loses this
disagreement; local irreducibility can retain all six elements. The run script
passes `--sorted ascending` so candidates stay ordered.

```sh
./solution < original.txt
./reference < original.txt
./run.sh
cat reduced.txt
```

The binaries target macOS 15+ ARM64. Rebuild with `../build.sh`.
`run.sh` finds installed `shrink` or the repository's Release/Debug build.
Set `SHRINK_BIN=/absolute/path/to/shrink` to select another executable.
An optional first argument chooses the output file: `./run.sh /tmp/median.txt`.
