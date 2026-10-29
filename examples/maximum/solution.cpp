#include <algorithm>
#include <cstdint>
#include <iostream>

int main() {
    std::size_t length;
    if (!(std::cin >> length) || length == 0) return 1;
    // Fault: zero is not an element of an array containing only negative values.
    std::int64_t maximum = 0;
    for (std::size_t i = 0; i < length; ++i) {
        std::int64_t value;
        if (!(std::cin >> value)) return 1;
        maximum = std::max(maximum, value);
    }
    std::cout << maximum << '\n';
}
