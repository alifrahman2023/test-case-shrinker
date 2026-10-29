#include <cstdint>
#include <iostream>

int main() {
    std::size_t length;
    std::uint32_t first, second;
    if (!(std::cin >> length >> first >> second) || length != 2) return 1;
    // Fault: the result needs more than 32 bits. Unsigned arithmetic wraps
    // modulo 2^32 on this Mac target; there is no undefined signed overflow.
    const std::uint32_t product = first * second;
    std::cout << product << '\n';
}
