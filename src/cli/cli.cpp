#include "cli.hpp"
#include "version.hpp"

#include <ostream>

namespace shrink::cli {
int run(std::span<const std::string_view> arguments, std::ostream& output,
        std::ostream& errors) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        output << "Usage: shrink --help | --version\n"
                  "\n"
                  "shrink: wrong-answer reduction for integer-array inputs.\n"
                  "\n"
                  "  --help     Show this help and exit.\n"
                  "  --version  Show the version and exit.\n"
                  "\n"
                  "This build provides informational commands only; reduction is not yet available.\n";
        return output ? 0 : 1;
    }
    if (arguments.size() == 1 && arguments.front() == "--version") {
        output << "shrink " << shrink::version() << '\n';
        return output ? 0 : 1;
    }
    errors << "Expected --help or --version. Reduction is not yet available.\n";
    return 2;
}
}
