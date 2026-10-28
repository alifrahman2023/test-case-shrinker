#include "cli.hpp"
#include "options.hpp"
#include "version.hpp"

#include <ostream>

namespace shrink::cli {
int run(std::span<const std::string_view> arguments, std::ostream& output,
        std::ostream& errors) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        output << "Usage: shrink [options]\n"
                  "\n"
                  "shrink: wrong-answer reduction for integer-array inputs.\n"
                  "\n"
                  "  --help     Show this help and exit.\n"
                  "  --version  Show the version and exit.\n"
                  "\n"
                  "Required: --input PATH --solution PATH --reference PATH\n"
                  "          --min-length N --max-length N --min-value N --max-value N\n"
                  "Optional:\n"
                  "  --output PATH             Result destination (default: reduced.txt).\n"
                  "  --sorted ascending|descending   Require nondecreasing/nonincreasing values.\n"
                  "  --unique                  Require distinct values.\n"
                  "  --solution-arg=VALUE      Append a solution argument (repeatable).\n"
                  "  --reference-arg=VALUE     Append a reference argument (repeatable).\n"
                  "  --solution-timeout TIME   Positive whole ms or s (default: 2s).\n"
                  "  --reference-timeout TIME  Positive whole ms or s (default: 10s).\n"
                  "  --time-limit TIME         Optional overall elapsed-time budget.\n"
                  "  --max-evaluations N       Optional nonnegative evaluation budget.\n"
                  "  --quiet                   Suppress periodic progress.\n"
                  "\n"
                  "Value options accept --option value or --option=value. Program arguments\n"
                  "beginning with '-' require the equals form. No shell interpretation.\n"
                  "Programs must be deterministic and create no descendant processes.\n"
                  "This build validates requests; program execution is not yet available.\n";
        return output ? 0 : 1;
    }
    if (arguments.size() == 1 && arguments.front() == "--version") {
        output << "shrink " << shrink::version() << '\n';
        return output ? 0 : 1;
    }
    auto prepared = prepare_invocation(arguments);
    if (auto* error = std::get_if<PreparationError>(&prepared)) {
        errors << error->operation;
        if (!error->path.empty()) errors << " (" << error->path << ')';
        errors << ": " << error->message << "\nNo verified result was produced; output was left unchanged.\n";
        return error->kind == PreparationErrorKind::invalid_configuration ? 2 : 1;
    }
    errors << "Input validated. Program execution is not yet available in this build.\n"
              "No verified result was produced; output was left unchanged.\n";
    return 1;
}
}
