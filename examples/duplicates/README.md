# Count distinct values

Given an integer array, print the number of different values in it.

**Input:** a length `n`, followed by exactly `n` integers. **Output:** one integer.

**Constraints:** `0 <= n <= 10`, `-100 <= a[i] <= 100`. The array may be
unsorted and contain duplicates.

The solution counts runs of equal neighbors, incorrectly assuming that equal
values are adjacent. The reference counts distinct values with a set.

The supplied input `[42, -7, 42, 9]` produces **4** from the solution and **3**
from the reference. A typical reduced input is `[0, 1, 0]`, with answers **3**
and **2**.

```sh
./solution < original.txt
./reference < original.txt
./run.sh
cat reduced.txt
```

The binaries target macOS 15+ ARM64. Rebuild with `../build.sh`.
`run.sh` finds installed `shrink` or the repository's Release/Debug build.
Set `SHRINK_BIN=/absolute/path/to/shrink` to select another executable.
An optional first argument chooses the output file: `./run.sh /tmp/distinct.txt`.
