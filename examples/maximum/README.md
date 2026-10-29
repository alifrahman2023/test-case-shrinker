# Find the maximum

Given a nonempty integer array, print its largest element.

**Input:** a length `n`, followed by exactly `n` integers. **Output:** one integer.

**Constraints:** `1 <= n <= 8`, `-100 <= a[i] <= 100`. The array may be
unsorted and contain duplicates.

The solution initializes its maximum to zero. It therefore prints zero for
arrays containing only negative values. The reference starts with the first
element of the array.

The supplied `[-8, -3, -12, -5]` produces **0** from the solution and **-3**
from the reference. A typical reduced input is `[-1]`, with answers **0** and
**-1**.

```sh
./solution < original.txt
./reference < original.txt
./run.sh
cat reduced.txt
```

The binaries target macOS 15+ ARM64. Rebuild with `../build.sh`.
`run.sh` finds installed `shrink` or the repository's Release/Debug build.
Set `SHRINK_BIN=/absolute/path/to/shrink` to select another executable.
An optional first argument chooses the output file: `./run.sh /tmp/maximum.txt`.
