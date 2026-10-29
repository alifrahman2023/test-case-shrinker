#pragma once

#include <iosfwd>
#include "reduction.hpp"
#include <span>
#include <string_view>

namespace shrink::cli {
// Internal invocation seam. Hosts supplying a runner retain it through pending
// cleanup. The terminal entry point owns its runner until process exit.
struct InvocationServices {
    Runner* runner = nullptr;
    CancellationToken cancellation;
    std::function<SaveResult(const std::filesystem::path&, const SaveRequest&)> save;
};
int run(std::span<const std::string_view> arguments, std::ostream& output,
        std::ostream& errors, const InvocationServices& services = {});
// Terminal-process adapter: translates SIGINT through ordinary polling code.
// Process-wide handler state precludes concurrent calls/reentry. Embedding
// callers use run() and supply their own cancellation token and runner.
int run_terminal(std::span<const std::string_view> arguments, std::ostream& output,
                 std::ostream& errors, InvocationServices services = {});
}
