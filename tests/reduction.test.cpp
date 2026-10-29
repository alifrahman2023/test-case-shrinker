#include "reduction.hpp"
#include "runner_backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <stdexcept>

namespace {
using namespace shrink;
using namespace shrink::detail;
class BaselineBackend final : public ExecutionBackend {
public:
    std::array<bool, 2> active{};
    std::vector<std::string> inputs;
    std::uint64_t clock = 10;
    std::uint64_t service_delay = 0;
    std::string solution_output = "1", reference_output = "1.0";
    std::array<int, 2> exit_codes{};
    std::optional<RunnerError> failure;
    std::uint64_t now_ms() override { return clock; }
    std::optional<RunnerError> launch(ProgramRole role, const ProgramSpec&, std::string_view input) override {
        inputs.emplace_back(input); active[program_index(role)] = true; return {};
    }
    std::vector<ExecutionEvent> service(CancellationToken, int) override {
        clock += service_delay;
        std::vector<ExecutionEvent> events;
        for (auto role : program_roles) {
            if (!active[program_index(role)]) continue;
            events.push_back({role, EventKind::setup_complete});
            events.push_back({role, EventKind::stdout_data, role == ProgramRole::solution ? solution_output : reference_output});
            events.push_back({role, EventKind::stdout_closed});
            events.push_back({role, EventKind::stderr_closed});
            events.push_back({role, EventKind::exited, {}, exit_codes[program_index(role)]});
            active[program_index(role)] = false;
        }
        if (failure) events.push_back({failure->program, EventKind::error, {}, failure->system_error, failure->code, failure->operation});
        return events;
    }
    std::optional<RunnerError> stop(ProgramRole) override { return {}; }
    bool pending(ProgramRole role) const override { return active[program_index(role)]; }
    PendingOwnership ownership(ProgramRole role) const override { return {role}; }
};
ReductionRequest request() {
    ReductionRequest value{" 3\t7 -2 7  ", {0, 100, -100, 100}, {"/s", {}}, {"/r", {}}, {}};
    value.limits.max_evaluations = 0;
    return value;
}
}
TEST_CASE("zero candidate budget verifies one normalized owning baseline without starting a round", "[reduction]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* observed = backend.get();
    Runner runner(std::move(backend));
    auto result = reduce(request(), runner);
    REQUIRE(result.outcome == RunOutcome::evaluation_limit);
    REQUIRE(result.best);
    REQUIRE(result.best->normalized_input == "3\n7 -2 7\n");
    REQUIRE(result.best->revision == 1);
    REQUIRE(result.baseline.evaluation->disagreement->token_position == 1);
    REQUIRE(result.best->evidence->execution.programs.solution.stdout_bytes == "1");
    REQUIRE(result.best->evidence->execution.programs.reference.stdout_bytes == "1.0");
    REQUIRE(result.statistics.baseline_evaluations == 1);
    REQUIRE(result.statistics.candidate_evaluations == 0);
    REQUIRE(result.statistics.round == RoundStatus::not_started);
    REQUIRE_FALSE(result.saving.configured);
    REQUIRE(observed->inputs == std::vector<std::string>{"3\n7 -2 7\n", "3\n7 -2 7\n"});
}
TEST_CASE("verified baseline is saved initially and finally with the same revision", "[reduction]") {
    auto backend = std::make_unique<BaselineBackend>();
    Runner runner(std::move(backend));
    std::vector<SaveKind> saves;
    std::vector<std::string> bytes;
    std::vector<std::uint64_t> revisions;
    RunHooks hooks;
    hooks.save = [&](const SaveRequest& save) -> SaveResult {
        saves.push_back(save.kind); bytes.emplace_back(save.normalized_input); revisions.push_back(save.revision);
        return {};
    };
    auto result = reduce(request(), runner, {}, hooks);
    REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::final});
    REQUIRE(bytes == std::vector<std::string>{"3\n7 -2 7\n", "3\n7 -2 7\n"});
    REQUIRE(revisions == std::vector<std::uint64_t>{1, 1});
    REQUIRE(result.saving.configured);
    REQUIRE(result.saving.last_saved_revision == 1);
    REQUIRE(result.saving.final_outcome == FinalSaveOutcome::succeeded);
}
TEST_CASE("initial save failure gets one final attempt and retains its contextual error", "[reduction]") {
    for (bool throws : {false, true}) {
        Runner runner(std::make_unique<BaselineBackend>());
        std::vector<SaveKind> saves;
        RunHooks hooks;
        hooks.save = [&](const SaveRequest& save) -> SaveResult {
            saves.push_back(save.kind);
            if (save.kind == SaveKind::initial) {
                if (throws) throw std::runtime_error("save callback");
                return SaveError{SaveErrorCode::replace, "rename", "/output", std::make_error_code(std::errc::permission_denied)};
            }
            return {};
        };
        auto result = reduce(request(), runner, {}, hooks);
        REQUIRE(result.best);
        REQUIRE(result.outcome == RunOutcome::operational_stop);
        REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::final});
        REQUIRE(result.saving.last_saved_revision == 1);
        REQUIRE(result.saving.final_outcome == FinalSaveOutcome::succeeded);
        REQUIRE(result.saving.failures.size() == 1);
        REQUIRE(result.saving.failures[0].error.code == (throws ? SaveErrorCode::callback_exception : SaveErrorCode::replace));
        REQUIRE(result.errors[0].kind == RunErrorKind::save);
    }
}
TEST_CASE("throwing baseline notifier preserves verified state and required saving", "[reduction]") {
    Runner runner(std::make_unique<BaselineBackend>());
    std::vector<SaveKind> saves;
    unsigned notifications = 0;
    auto caller = std::this_thread::get_id();
    RunHooks hooks;
    hooks.notify = [&](const RunEvent& event) {
        REQUIRE(std::this_thread::get_id() == caller);
        REQUIRE(event.kind == RunEventKind::baseline_verified);
        REQUIRE(event.best.normalized_input == "3\n7 -2 7\n");
        ++notifications;
        throw std::runtime_error("notification");
    };
    hooks.save = [&](const SaveRequest& save) -> SaveResult { saves.push_back(save.kind); return {}; };
    auto result = reduce(request(), runner, {}, hooks);
    REQUIRE(result.best);
    REQUIRE(notifications == 1);
    REQUIRE(result.outcome == RunOutcome::operational_stop);
    REQUIRE(result.errors[0].kind == RunErrorKind::notification);
    REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::final});
}
TEST_CASE("overall deadline interrupts baseline and prevents any save", "[reduction]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* observed = backend.get();
    observed->service_delay = 100;
    Runner runner(std::move(backend));
    auto input = request(); input.limits.time_limit = std::chrono::milliseconds(50);
    unsigned saves = 0;
    RunHooks hooks; hooks.save = [&](const SaveRequest&) -> SaveResult { ++saves; return {}; };
    auto result = reduce(std::move(input), runner, {}, hooks);
    REQUIRE(result.outcome == RunOutcome::time_limit);
    REQUIRE(result.observed_reasons.time_limit);
    REQUIRE_FALSE(result.observed_reasons.cancellation);
    REQUIRE_FALSE(result.best);
    REQUIRE(saves == 0);
    REQUIRE(result.statistics.elapsed_ms == 100);
    REQUIRE(result.baseline.status == BaselineStatus::interrupted);
}
TEST_CASE("ordinary rejected baselines and cleanup errors never save or establish a best", "[reduction]") {
    for (int scenario = 0; scenario < 4; ++scenario) {
        auto backend = std::make_unique<BaselineBackend>();
        if (scenario == 0) backend->reference_output = "  1 \n";
        if (scenario == 1) backend->exit_codes[0] = 7;
        if (scenario == 2) backend->exit_codes[1] = 127;
        if (scenario == 3) backend->failure = RunnerError{RunnerErrorCode::cleanup, {}, EIO, "cleanup"};
        Runner runner(std::move(backend));
        unsigned saves = 0, notifications = 0;
        RunHooks hooks;
        hooks.save = [&](const SaveRequest&) -> SaveResult { ++saves; return {}; };
        hooks.notify = [&](const RunEvent&) { ++notifications; };
        auto result = reduce(request(), runner, {}, hooks);
        REQUIRE_FALSE(result.best);
        REQUIRE(saves == 0);
        REQUIRE(notifications == 0);
        REQUIRE(result.outcome == (scenario == 3 ? RunOutcome::operational_stop : RunOutcome::baseline_rejected));
        if (scenario == 0) REQUIRE(result.baseline.status == BaselineStatus::agreement);
        if (scenario == 1) REQUIRE(result.baseline.status == BaselineStatus::solution_unsuccessful);
        if (scenario == 2) REQUIRE(result.baseline.status == BaselineStatus::reference_unsuccessful);
        if (scenario == 3) REQUIRE(result.errors[0].kind == RunErrorKind::cleanup);
    }
}
TEST_CASE("invalid and pre-cancelled requests launch no programs and preserve absence of a best", "[reduction]") {
    for (bool invalid : {true, false}) {
        auto backend = std::make_unique<BaselineBackend>(); auto* observed = backend.get();
        Runner runner(std::move(backend));
        auto input = request(); if (invalid) input.input_text = "3 1";
        CancellationSource source; source.request_stop();
        auto result = reduce(std::move(input), runner, source.token());
        REQUIRE(observed->inputs.empty());
        REQUIRE_FALSE(result.best);
        REQUIRE(result.outcome == (invalid ? RunOutcome::invalid_request : RunOutcome::cancelled));
    }
}
TEST_CASE("final save exceptions fail once while retaining verified in-memory recovery", "[reduction]") {
    Runner runner(std::make_unique<BaselineBackend>());
    unsigned calls = 0;
    RunHooks hooks;
    hooks.save = [&](const SaveRequest& save) -> SaveResult {
        ++calls;
        if (save.kind == SaveKind::final) throw std::runtime_error("final");
        return {};
    };
    auto result = reduce(request(), runner, {}, hooks);
    REQUIRE(calls == 2);
    REQUIRE(result.best->normalized_input == "3\n7 -2 7\n");
    REQUIRE(result.outcome == RunOutcome::evaluation_limit);
    REQUIRE(result.saving.last_saved_revision == 1);
    REQUIRE(result.saving.final_outcome == FinalSaveOutcome::failed);
    REQUIRE(result.errors[0].kind == RunErrorKind::save);
}
TEST_CASE("cancellation during initial saving retains the best and still attempts final saving", "[reduction]") {
    Runner runner(std::make_unique<BaselineBackend>());
    CancellationSource source;
    unsigned calls = 0;
    RunHooks hooks;
    hooks.save = [&](const SaveRequest&) -> SaveResult { ++calls; source.request_stop(); return {}; };
    auto result = reduce(request(), runner, source.token(), hooks);
    REQUIRE(result.best);
    REQUIRE(calls == 2);
    REQUIRE(result.outcome == RunOutcome::cancelled);
    REQUIRE(result.observed_reasons.cancellation);
}
TEST_CASE("returned baseline and best evidence outlive the request and runner", "[reduction]") {
    auto result = [] {
        Runner runner(std::make_unique<BaselineBackend>());
        return reduce(request(), runner);
    }();
    REQUIRE(result.best->values == std::vector<std::int64_t>{7, -2, 7});
    REQUIRE(result.best->normalized_input == "3\n7 -2 7\n");
    REQUIRE(result.baseline.evaluation->execution.programs.solution.stdout_bytes == "1");
    REQUIRE(result.best->evidence->execution.programs.reference.stdout_bytes == "1.0");
    REQUIRE(result.best->evidence->disagreement->reference.offset == 0);
    REQUIRE(result.best->evidence->disagreement->reference.length == 3);
}
TEST_CASE("operational errors retain simultaneous overall deadline observations", "[reduction]") {
    auto backend = std::make_unique<BaselineBackend>();
    backend->service_delay = 100;
    backend->failure = RunnerError{RunnerErrorCode::cleanup, {}, EIO, "cleanup"};
    Runner runner(std::move(backend));
    auto input = request(); input.limits.time_limit = std::chrono::milliseconds(50);
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.outcome == RunOutcome::operational_stop);
    REQUIRE(result.observed_reasons.time_limit);
    REQUIRE_FALSE(result.best);
    REQUIRE(result.errors[0].kind == RunErrorKind::cleanup);
}
TEST_CASE("overall budget includes initial save time and final save still runs", "[reduction]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* clock = backend.get();
    Runner runner(std::move(backend));
    auto input = request(); input.limits.time_limit = std::chrono::milliseconds(50);
    unsigned saves = 0;
    RunHooks hooks;
    hooks.save = [&](const SaveRequest&) -> SaveResult { ++saves; clock->clock += 100; return {}; };
    auto result = reduce(std::move(input), runner, {}, hooks);
    REQUIRE(saves == 2);
    REQUIRE(result.best);
    REQUIRE(result.outcome == RunOutcome::time_limit);
    REQUIRE(result.statistics.elapsed_ms == 200);
    REQUIRE(result.saving.final_outcome == FinalSaveOutcome::succeeded);
}
