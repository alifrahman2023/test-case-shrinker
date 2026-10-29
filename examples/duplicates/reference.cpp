#include <cstdint>
#include <iostream>
#include <set>

int main() {
    std::size_t length;
    if (!(std::cin >> length)) return 1;
    std::set<std::int64_t> distinct;
    for (std::size_t index = 0; index < length; ++index) {
        std::int64_t value;
        if (!(std::cin >> value)) return 1;
        distinct.insert(value);
    }
    std::cout << distinct.size() << '\n';
}
