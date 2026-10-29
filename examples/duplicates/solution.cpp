#include <cstdint>
#include <iostream>

// Fault: counting runs only counts distinct values when equal values are adjacent.
int main() {
    std::size_t length;
    if (!(std::cin >> length)) return 1;
    std::int64_t previous = 0;
    std::size_t count = 0;
    for (std::size_t index = 0; index < length; ++index) {
        std::int64_t value;
        if (!(std::cin >> value)) return 1;
        if (index == 0 || value != previous) ++count;
        previous = value;
    }
    std::cout << count << '\n';
}
