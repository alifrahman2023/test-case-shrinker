#include <cstdint>
#include <iostream>

int main() {
    std::size_t length;
    std::uint64_t first, second;
    if (!(std::cin >> length >> first >> second) || length != 2) return 1;
    // Declared values are at most 100000, so their product fits in 64 bits.
    std::cout << first * second << '\n';
}
