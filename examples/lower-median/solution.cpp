#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::size_t length;
    if (!(std::cin >> length) || length == 0) return 1;
    std::vector<std::int64_t> values(length);
    for (auto& value : values) if (!(std::cin >> value)) return 1;
    // Fault: for even lengths this selects the upper, rather than lower, median.
    std::cout << values[length / 2] << '\n';
}
