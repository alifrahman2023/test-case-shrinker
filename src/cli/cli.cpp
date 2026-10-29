#include "cli.hpp"
#include "options.hpp"
#include "version.hpp"
#include "reduction.hpp"
#include "saving.hpp"
#include "presentation.hpp"

#include <ostream>

namespace shrink::cli {
int run(std::span<const std::string_view> arguments, std::ostream& output,
        std::ostream& errors, const InvocationServices& services) {
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
                  "Use --max-evaluations 0 to verify and save a baseline; search is not yet available.\n";
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
    auto invocation = std::get<Invocation>(std::move(prepared));
    auto owned_runner = services.runner ? std::unique_ptr<Runner>{} : std::make_unique<Runner>();
    auto& runner = services.runner ? *services.runner : *owned_runner;
    RunHooks hooks;
    hooks.save = [&](const SaveRequest& save) {
        return services.save ? services.save(invocation.output_path, save) : save_atomic(invocation.output_path, save);
    };
    auto result = reduce(std::move(invocation.core.request), runner, services.cancellation, hooks);
    // The terminal adapter returns to main and exits after reporting unresolved
    // ownership. Keep that runner alive until process exit rather than invoking
    // its destructor contract check. Continuing hosts supply their own runner.
    if (owned_runner && runner.has_pending_ownership()) (void)owned_runner.release();
    const int status = present_result(result, invocation.output_path, output, errors);
    // Recovery/presentation can block after reduce() has returned. Preserve a
    // cancellation observed there without overriding an operational failure or
    // a search that already established completion.
    if (services.cancellation.stop_requested() && !result.observed_reasons.cancellation) {
        errors << "Observed: caller cancellation during terminal output.\n";
        if (status == 3 || status == 4) return errors ? 130 : 1;
    }
    return status;
}
}
