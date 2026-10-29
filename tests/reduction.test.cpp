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
    std::function<void(std::string_view)> on_evaluation;
    std::uint64_t now_ms() override { return clock; }
    std::optional<RunnerError> launch(ProgramRole role, const ProgramSpec&, std::string_view input) override {
        if (role == ProgramRole::solution && on_evaluation) on_evaluation(input);
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
    for (auto status : {BaselineStatus::agreement, BaselineStatus::solution_unsuccessful,
                        BaselineStatus::reference_unsuccessful, BaselineStatus::operational_failure}) {
        auto backend = std::make_unique<BaselineBackend>();
        if (status == BaselineStatus::agreement) backend->reference_output = "  1 \n";
        if (status == BaselineStatus::solution_unsuccessful) backend->exit_codes[0] = 7;
        if (status == BaselineStatus::reference_unsuccessful) backend->exit_codes[1] = 127;
        if (status == BaselineStatus::operational_failure) backend->failure = RunnerError{RunnerErrorCode::cleanup, {}, EIO, "cleanup"};
        Runner runner(std::move(backend));
        unsigned saves = 0, notifications = 0;
        RunHooks hooks;
        hooks.save = [&](const SaveRequest&) -> SaveResult { ++saves; return {}; };
        hooks.notify = [&](const RunEvent&) { ++notifications; };
        auto result = reduce(request(), runner, {}, hooks);
        REQUIRE_FALSE(result.best);
        REQUIRE(saves == 0);
        REQUIRE(notifications == 0);
        REQUIRE(result.outcome == (status == BaselineStatus::operational_failure ? RunOutcome::operational_stop : RunOutcome::baseline_rejected));
        REQUIRE(result.baseline.status == status);
        if (status == BaselineStatus::operational_failure) REQUIRE(result.errors[0].kind == RunErrorKind::cleanup);
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
TEST_CASE("cancellation during final saving returns a cancelled run with verified recovery", "[reduction][cancellation]") {
    Runner runner(std::make_unique<BaselineBackend>());
    CancellationSource source;
    std::vector<SaveKind> saves;
    RunHooks hooks;
    hooks.save = [&](const SaveRequest& save) -> SaveResult {
        saves.push_back(save.kind);
        if (save.kind == SaveKind::final) source.request_stop();
        return {};
    };
    auto result = reduce(request(), runner, source.token(), hooks);
    REQUIRE(result.outcome == RunOutcome::cancelled);
    REQUIRE(result.observed_reasons.cancellation);
    REQUIRE(result.observed_reasons.evaluation_limit);
    REQUIRE(result.best->normalized_input == "3\n7 -2 7\n");
    REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::final});
    REQUIRE(result.saving.final_outcome == FinalSaveOutcome::succeeded);
    REQUIRE(result.statistics.candidate_evaluations == 0);
    REQUIRE(result.statistics.round == RoundStatus::not_started);
}
TEST_CASE("cancelled callback failures preserve verified state and every stop reason", "[reduction][cancellation]") {
    enum class Fault { initial_save, final_save, notifier };
    for (auto fault : {Fault::initial_save, Fault::final_save, Fault::notifier}) {
        Runner runner(std::make_unique<BaselineBackend>());
        CancellationSource source;
        std::vector<SaveKind> saves;
        unsigned notifications = 0;
        RunHooks hooks;
        hooks.notify = [&](const RunEvent&) {
            ++notifications;
            if (fault == Fault::notifier) { source.request_stop(); throw std::runtime_error("notification failure"); }
        };
        hooks.save = [&](const SaveRequest& save) -> SaveResult {
            saves.push_back(save.kind);
            if ((fault == Fault::initial_save && save.kind == SaveKind::initial) ||
                (fault == Fault::final_save && save.kind == SaveKind::final)) {
                source.request_stop();
                throw std::runtime_error("save failure");
            }
            return {};
        };
        auto result = reduce(request(), runner, source.token(), hooks);
        REQUIRE(result.observed_reasons.cancellation);
        REQUIRE(result.best->normalized_input == "3\n7 -2 7\n");
        REQUIRE(result.baseline.status == BaselineStatus::verified);
        REQUIRE(result.statistics.candidate_evaluations == 0);
        REQUIRE(result.statistics.round == RoundStatus::not_started);
        REQUIRE(result.errors.size() == 1);
        REQUIRE(result.errors[0].kind == (fault == Fault::notifier ? RunErrorKind::notification : RunErrorKind::save));
        REQUIRE(notifications == 1);
        REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::final});
        REQUIRE(result.saving.final_outcome == (fault == Fault::final_save ? FinalSaveOutcome::failed : FinalSaveOutcome::succeeded));
        REQUIRE_FALSE(runner.has_pending_ownership());
    }
}
TEST_CASE("budgeted removal accepts smaller valid inputs and reuses acceptance evidence", "[reduction][removal]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* observed = backend.get();
    Runner runner(std::move(backend));
    auto input = request(); input.limits.max_evaluations = 2;
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.outcome == RunOutcome::evaluation_limit);
    REQUIRE(result.best->normalized_input == "1\n7\n");
    REQUIRE(result.best->revision == 3);
    REQUIRE(result.statistics.candidate_evaluations == 2);
    REQUIRE(result.statistics.acceptances == 2);
    REQUIRE(result.statistics.round == RoundStatus::partial);
    REQUIRE(observed->inputs == std::vector<std::string>{"3\n7 -2 7\n", "3\n7 -2 7\n", "2\n-2 7\n", "2\n-2 7\n", "1\n7\n", "1\n7\n"});
}
TEST_CASE("acceptance notifications expose revisions on caller thread and checkpoint latest pending best", "[reduction][removal]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* clock = backend.get();
    clock->service_delay = 400;
    Runner runner(std::move(backend));
    auto input = request(); input.input_text = "8 1 2 3 4 5 6 7 8"; input.constraints.min_length = 1; input.limits.max_evaluations = 3;
    std::vector<std::pair<SaveKind, std::uint64_t>> saves;
    std::vector<std::string> accepted;
    auto caller = std::this_thread::get_id();
    RunHooks hooks;
    hooks.notify = [&](const RunEvent& event) {
        REQUIRE(std::this_thread::get_id() == caller);
        if (event.kind != RunEventKind::baseline_verified && event.best.revision > 1)
            accepted.push_back(event.best.normalized_input);
    };
    hooks.save = [&](const SaveRequest& save) -> SaveResult { saves.emplace_back(save.kind, save.revision); return {}; };
    auto result = reduce(std::move(input), runner, {}, hooks);
    REQUIRE(result.statistics.acceptances == 3);
    REQUIRE_FALSE(accepted.empty());
    REQUIRE(saves == std::vector<std::pair<SaveKind, std::uint64_t>>{{SaveKind::initial, 1}, {SaveKind::periodic, 4}, {SaveKind::final, 4}});
    REQUIRE(result.saving.last_saved_revision == 4);
}
TEST_CASE("acceptance notifier failure stops search and retains budget observation with final recovery", "[reduction][removal]") {
    Runner runner(std::make_unique<BaselineBackend>());
    auto input = request(); input.limits.max_evaluations = 1;
    unsigned notices = 0;
    std::vector<std::uint64_t> saved;
    RunHooks hooks;
    hooks.notify = [&](const RunEvent& event) {
        ++notices;
        if (event.kind == RunEventKind::acceptance) throw std::runtime_error("acceptance");
    };
    hooks.save = [&](const SaveRequest& save) -> SaveResult { saved.push_back(save.revision); return {}; };
    auto result = reduce(std::move(input), runner, {}, hooks);
    REQUIRE(result.observed_reasons.evaluation_limit);
    REQUIRE(result.outcome == RunOutcome::operational_stop);
    REQUIRE(result.errors.size() == 1);
    REQUIRE(result.errors[0].kind == RunErrorKind::notification);
    REQUIRE(notices == 2);
    REQUIRE(saved == std::vector<std::uint64_t>{1, 2});
    REQUIRE(result.best->normalized_input == "2\n-2 7\n");
}
TEST_CASE("periodic save failures and exceptions retain best and retry exactly once at final", "[reduction][removal]") {
    for (bool throws : {false, true}) {
        auto backend = std::make_unique<BaselineBackend>(); backend->service_delay = 1000;
        Runner runner(std::move(backend));
        auto input = request(); input.limits.max_evaluations = 2;
        std::vector<SaveKind> saves;
        RunHooks hooks;
        hooks.save = [&](const SaveRequest& save) -> SaveResult {
            saves.push_back(save.kind);
            if (save.kind == SaveKind::periodic) {
                if (throws) throw std::runtime_error("checkpoint");
                return SaveError{SaveErrorCode::replace, "checkpoint replace", {}, {}};
            }
            return {};
        };
        auto result = reduce(std::move(input), runner, {}, hooks);
        REQUIRE(result.outcome == RunOutcome::operational_stop);
        REQUIRE(result.statistics.candidate_evaluations == 1);
        REQUIRE(result.best->revision == 2);
        REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::periodic, SaveKind::final});
        REQUIRE(result.saving.last_saved_revision == 2);
        REQUIRE(result.saving.failures.size() == 1);
        REQUIRE(result.saving.failures[0].kind == SaveKind::periodic);
    }
}
TEST_CASE("removal sweeps left to right halves chunks and counts distinct inconclusive texts", "[reduction][removal]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    script->on_evaluation = [script](std::string_view text) {
        if (text.starts_with("4\n")) return;
        script->exit_codes[1] = 7;
    };
    Runner runner(std::move(backend));
    auto input = request(); input.input_text = "4 1 1 1 1"; input.limits.max_evaluations = 5;
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.outcome == RunOutcome::evaluation_limit);
    REQUIRE(result.statistics.inconclusive_executions == 5);
    REQUIRE(result.statistics.distinct_inconclusive_candidates == 2);
    REQUIRE(result.statistics.acceptances == 0);
    REQUIRE(result.best->revision == 1);
    REQUIRE(script->inputs[2] == "2\n1 1\n");
    REQUIRE(script->inputs[6] == "3\n1 1 1\n");
}
TEST_CASE("invalid removals consume no budget and removal exhaustion never claims completion", "[reduction][removal]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    Runner runner(std::move(backend));
    auto input = request(); input.constraints.min_length = 3; input.limits.max_evaluations = 1;
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.outcome == RunOutcome::operational_stop);
    REQUIRE(result.errors[0].kind == RunErrorKind::search_unavailable);
    REQUIRE(result.statistics.round == RoundStatus::partial);
    REQUIRE(result.statistics.candidate_evaluations == 0);
    REQUIRE(script->inputs.size() == 2);
    REQUIRE(result.best->revision == 1);
}
TEST_CASE("cancellation during invalid candidate processing launches no candidates and saves final", "[reduction][removal]") {
    auto backend = std::make_unique<BaselineBackend>();
    Runner runner(std::move(backend));
    auto input = request(); input.input_text = "8 1 2 3 4 5 6 7 8";
    input.constraints.min_length = 8; input.limits.max_evaluations.reset();
    bool verified = false; unsigned polls = 0, saves = 0;
    CancellationToken cancellation;
    cancellation = cancellation.with_stop_check([&] { return verified && ++polls >= 10; });
    RunHooks hooks;
    hooks.notify = [&](const RunEvent&) { verified = true; };
    hooks.save = [&](const SaveRequest&) -> SaveResult { ++saves; return {}; };
    auto result = reduce(std::move(input), runner, cancellation, hooks);
    REQUIRE(result.outcome == RunOutcome::cancelled);
    REQUIRE(result.observed_reasons.cancellation);
    REQUIRE(result.statistics.candidate_evaluations == 0);
    REQUIRE(saves == 2);
}
TEST_CASE("cancelled active removal is discarded and candidate errors retain verified recovery", "[reduction][removal]") {
    for (bool error : {false, true}) {
        auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
        CancellationSource source;
        script->on_evaluation = [&](std::string_view text) {
            if (text.starts_with("3\n")) return;
            source.request_stop();
            if (error) script->failure = RunnerError{RunnerErrorCode::io, {}, EIO, "candidate io"};
        };
        Runner runner(std::move(backend));
        auto input = request(); input.limits.max_evaluations = 1;
        unsigned saves = 0;
        RunHooks hooks; hooks.save = [&](const SaveRequest&) -> SaveResult { ++saves; return {}; };
        auto result = reduce(std::move(input), runner, source.token(), hooks);
        REQUIRE(result.observed_reasons.cancellation);
        REQUIRE(result.observed_reasons.evaluation_limit);
        REQUIRE(result.outcome == (error ? RunOutcome::operational_stop : RunOutcome::cancelled));
        REQUIRE(result.best->revision == 1);
        REQUIRE(result.statistics.acceptances == 0);
        REQUIRE(result.statistics.candidate_evaluations == 1);
        REQUIRE(saves == 2);
    }
}
TEST_CASE("checkpoint throttle uses successful save time and final save bypasses pending suppression", "[reduction][removal]") {
    for (std::uint64_t delay : {999, 1000}) {
        auto backend = std::make_unique<BaselineBackend>(); backend->service_delay = delay;
        Runner runner(std::move(backend));
        auto input = request(); input.limits.max_evaluations = 1;
        std::vector<SaveKind> saves;
        RunHooks hooks; hooks.save = [&](const SaveRequest& save) -> SaveResult { saves.push_back(save.kind); return {}; };
        auto result = reduce(std::move(input), runner, {}, hooks);
        REQUIRE(saves == (delay == 999 ? std::vector{SaveKind::initial, SaveKind::final}
            : std::vector{SaveKind::initial, SaveKind::periodic, SaveKind::final}));
        REQUIRE(result.saving.last_saved_revision == 2);
    }
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    script->service_delay = 1000;
    script->on_evaluation = [script](std::string_view text) { if (!text.starts_with("3\n")) script->reference_output = "1"; };
    Runner runner(std::move(backend));
    auto input = request(); input.limits.max_evaluations = 2;
    std::vector<SaveKind> saves;
    RunHooks hooks; hooks.save = [&](const SaveRequest& save) -> SaveResult { saves.push_back(save.kind); return {}; };
    auto result = reduce(std::move(input), runner, {}, hooks);
    REQUIRE(result.statistics.lost_executions == 2);
    REQUIRE(result.statistics.acceptances == 0);
    REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::final});
}
TEST_CASE("operational candidate evidence is owned without replacing the verified best", "[reduction][removal]") {
    auto result = [] {
        auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
        script->on_evaluation = [script](std::string_view text) {
            if (!text.starts_with("3\n")) script->failure = RunnerError{RunnerErrorCode::io, {}, EIO, "candidate io"};
        };
        Runner runner(std::move(backend));
        auto input = request(); input.limits.max_evaluations = 2;
        return reduce(std::move(input), runner);
    }();
    REQUIRE(result.outcome == RunOutcome::operational_stop);
    REQUIRE(result.best->revision == 1);
    REQUIRE(result.operational_evaluation);
    REQUIRE(result.operational_evaluation->execution.errors[0].operation == "candidate io");
}
TEST_CASE("removal accepts repeatable valid strict decreases with observed preservation", "[reduction][removal]") {
    std::vector<std::string> prior;
    for (int run = 0; run < 2; ++run) {
        auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
        InputConstraints constraints{1, 6, 1, 7, Ordering::ascending, true};
        script->on_evaluation = [script, constraints](std::string_view text) {
            auto parsed = parse_input(text, constraints);
            REQUIRE(std::holds_alternative<std::vector<std::int64_t>>(parsed));
            const auto& values = std::get<std::vector<std::int64_t>>(parsed);
            script->reference_output = std::find(values.begin(), values.end(), 1) != values.end() ? "2" : "1";
        };
        Runner runner(std::move(backend));
        auto input = request(); input.input_text = "6 1 2 3 4 5 7"; input.constraints = constraints; input.limits.max_evaluations = 4;
        std::vector<std::string> accepted;
        std::size_t previous_length = 6;
        RunHooks hooks; hooks.notify = [&](const RunEvent& event) {
            if (event.kind != RunEventKind::acceptance) return;
            REQUIRE(event.best.values.size() < previous_length);
            REQUIRE(event.best.values.front() == 1);
            REQUIRE(event.best.evidence->outcome == CandidateOutcome::preserved);
            previous_length = event.best.values.size();
            accepted.push_back(event.best.normalized_input);
        };
        auto result = reduce(std::move(input), runner, {}, hooks);
        REQUIRE(result.outcome == RunOutcome::evaluation_limit);
        REQUIRE(result.statistics.lost_executions == 2);
        REQUIRE(accepted == std::vector<std::string>{"3\n1 2 3\n", "2\n1 3\n"});
        if (run) REQUIRE(accepted == prior);
        prior = accepted;
    }
}
