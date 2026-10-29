#include <cstdint>
#include <iostream>

int main() {
    std::size_t length;
    if (!(std::cin >> length)) return 1;
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < length; ++i) {
        std::int64_t value;
        if (!(std::cin >> value)) return 1;
        // Fault: the last element is read, but never included in the sum.
        if (i + 1 < length) sum += value;
    }
    std::cout << sum << '\n';
}
