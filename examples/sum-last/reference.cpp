#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>

int main() {
    std::size_t length;
    if (!(std::cin >> length)) return 1;
    std::vector<std::int64_t> values(length);
    for (auto& value : values) if (!(std::cin >> value)) return 1;
    std::cout << std::accumulate(values.begin(), values.end(), std::int64_t{0}) << '\n';
}
