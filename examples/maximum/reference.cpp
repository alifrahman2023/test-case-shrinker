#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::size_t length;
    if (!(std::cin >> length) || length == 0) return 1;
    std::vector<std::int64_t> values(length);
    for (auto& value : values) if (!(std::cin >> value)) return 1;
    std::cout << *std::max_element(values.begin(), values.end()) << '\n';
}
