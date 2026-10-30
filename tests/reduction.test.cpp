#include "reduction.hpp"
#include "runner_backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <algorithm>
#include <set>
#include <limits>
#include <stdexcept>
#include <sstream>

namespace {
using namespace shrink;
using namespace shrink::detail;
// Test-side decoding keeps the ranking/local-check oracle independent of the
// typed representation. Only bytes cross this seam.
std::vector<std::int64_t> observed_values(std::string_view text) {
    std::istringstream stream{std::string(text)};
    std::size_t length = 0;
    REQUIRE(static_cast<bool>(stream >> length));
    std::vector<std::int64_t> values(length);
    for (auto& value : values) REQUIRE(static_cast<bool>(stream >> value));
    std::string extra;
    REQUIRE_FALSE(static_cast<bool>(stream >> extra));
    return values;
}
std::string oracle_text(const std::vector<std::int64_t>& values) {
    std::ostringstream stream;
    stream << values.size() << '\n';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) stream << ' ';
        stream << values[index];
    }
    stream << '\n';
    return stream.str();
}
std::vector<std::int64_t> parsed_values(std::string_view text, const InputConstraints& constraints) {
    auto parsed = parse_input(text, constraints);
    REQUIRE(std::holds_alternative<ArrayNode>(parsed));
    const auto values = observed_values(text);
    REQUIRE(serialize_input(std::get<ArrayNode>(parsed)) == oracle_text(values));
    return values;
}
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
    std::function<void()> on_clock;
    std::uint64_t now_ms() override { if (on_clock) on_clock(); return clock; }
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
    REQUIRE(observed_values(result.best->normalized_input) == std::vector<std::int64_t>{7, -2, 7});
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
    auto result = reduce(std::move(input), runner, {}, {}, {.enabled = false});
    REQUIRE(result.outcome == RunOutcome::evaluation_limit);
    REQUIRE(result.statistics.inconclusive_executions == 5);
    REQUIRE(result.statistics.distinct_inconclusive_candidates == 2);
    REQUIRE(result.statistics.acceptances == 0);
    REQUIRE(result.best->revision == 1);
    REQUIRE(script->inputs[2] == "2\n1 1\n");
    REQUIRE(script->inputs[6] == "3\n1 1 1\n");
}
TEST_CASE("invalid removals consume no budget before value simplification", "[reduction][removal]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    Runner runner(std::move(backend));
    auto input = request(); input.constraints.min_length = 3; input.limits.max_evaluations = 1;
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.outcome == RunOutcome::evaluation_limit);
    REQUIRE(result.errors.empty());
    REQUIRE(result.statistics.round == RoundStatus::partial);
    REQUIRE(result.statistics.candidate_evaluations == 1);
    REQUIRE(script->inputs.size() == 4);
    REQUIRE(result.best->revision == 2);
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
            const auto values = parsed_values(text, constraints);
            script->reference_output = std::find(values.begin(), values.end(), 1) != values.end() ? "2" : "1";
        };
        Runner runner(std::move(backend));
        auto input = request(); input.input_text = "6 1 2 3 4 5 7"; input.constraints = constraints; input.limits.max_evaluations = 4;
        std::vector<std::string> accepted;
        std::size_t previous_length = 6;
        RunHooks hooks; hooks.notify = [&](const RunEvent& event) {
            if (event.kind != RunEventKind::acceptance) return;
            const auto current = observed_values(event.best.normalized_input);
            REQUIRE(current.size() < previous_length);
            REQUIRE(current.front() == 1);
            REQUIRE(event.best.evidence->outcome == CandidateOutcome::preserved);
            previous_length = current.size();
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
TEST_CASE("progress elapsed time includes the preceding checkpoint callback", "[reduction][removal]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* clock = backend.get();
    clock->service_delay = 1000;
    Runner runner(std::move(backend));
    auto input = request(); input.limits.max_evaluations = 1;
    std::vector<std::uint64_t> progress;
    RunHooks hooks;
    hooks.save = [&](const SaveRequest& save) -> SaveResult {
        if (save.kind == SaveKind::periodic) clock->clock += 500;
        return {};
    };
    hooks.notify = [&](const RunEvent& event) {
        if (event.kind == RunEventKind::progress) progress.push_back(event.statistics.elapsed_ms);
    };
    auto result = reduce(std::move(input), runner, {}, hooks);
    REQUIRE(progress == std::vector<std::uint64_t>{1000, 2500});
    REQUIRE(result.statistics.elapsed_ms == 2500);
}
TEST_CASE("completion at the evaluation limit wins once all remaining checks are invalid", "[reduction][removal]") {
    Runner runner(std::make_unique<BaselineBackend>());
    auto input = request(); input.input_text = "1 7"; input.limits.max_evaluations = 1;
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.best->normalized_input == "0\n\n");
    REQUIRE(result.outcome == RunOutcome::completed);
    REQUIRE(result.observed_reasons.evaluation_limit);
    REQUIRE(result.errors.empty());
    REQUIRE(result.statistics.round == RoundStatus::complete);
}
TEST_CASE("empty and already simplest fixed-length inputs complete without candidate executions", "[reduction][values]") {
    for (const auto& text : {"0", "1 0"}) {
        Runner runner(std::make_unique<BaselineBackend>());
        auto input = request(); input.input_text = text; input.constraints = {text[0] - '0', 1, 0, 0};
        input.limits.max_evaluations.reset();
        auto result = reduce(std::move(input), runner);
        REQUIRE(result.outcome == RunOutcome::completed);
        REQUIRE(result.statistics.round == RoundStatus::complete);
        REQUIRE(result.statistics.candidate_evaluations == 0);
        REQUIRE(result.errors.empty());
    }
}
TEST_CASE("duplicate-wide simplification preserves a failure that individual changes lose", "[reduction][values]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    script->on_evaluation = [script](std::string_view text) {
        auto values = parsed_values(text, {0, 3, -100, 100});
        script->reference_output = values.size() == 2 && values[0] == values[1] ? "2" : "1";
    };
    Runner runner(std::move(backend));
    auto input = request(); input.input_text = "2 42 42"; input.limits.max_evaluations.reset();
    std::vector<std::string> accepted;
    RunHooks hooks; hooks.notify = [&](const RunEvent& event) {
        if (event.kind == RunEventKind::acceptance) accepted.push_back(event.best.normalized_input);
    };
    auto result = reduce(std::move(input), runner, {}, hooks);
    REQUIRE(result.outcome == RunOutcome::completed);
    REQUIRE(result.best->normalized_input == "2\n0 0\n");
    REQUIRE(accepted == std::vector<std::string>{"2\n0 0\n"});
    REQUIRE(result.statistics.round == RoundStatus::complete);
}
TEST_CASE("magnitude simplification uses a unit step and binary search after simplest loses", "[reduction][values]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    script->on_evaluation = [script](std::string_view text) {
        auto values = parsed_values(text, {1, 1, 0, 100});
        script->reference_output = values[0] >= 37 ? "2" : "1";
    };
    Runner runner(std::move(backend));
    auto input = request(); input.input_text = "1 100"; input.constraints = {1, 1, 0, 100}; input.limits.max_evaluations.reset();
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.outcome == RunOutcome::completed);
    REQUIRE(observed_values(result.best->normalized_input) == std::vector<std::int64_t>{37});
    REQUIRE(result.statistics.candidate_evaluations < 20);
    REQUIRE(result.statistics.round == RoundStatus::complete);
}
TEST_CASE("positive counterpart is tried even when magnitude simplification loses", "[reduction][values]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    script->on_evaluation = [script](std::string_view text) {
        auto values = parsed_values(text, {1, 1, -5, 5});
        script->reference_output = values[0] == -5 || values[0] == 5 ? "2" : "1";
    };
    Runner runner(std::move(backend));
    auto input = request(); input.input_text = "1 -5"; input.constraints = {1, 1, -5, 5}; input.limits.max_evaluations.reset();
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.outcome == RunOutcome::completed);
    REQUIRE(observed_values(result.best->normalized_input) == std::vector<std::int64_t>{5});
}
TEST_CASE("individual repeated-element changes can succeed after duplicate-wide changes lose", "[reduction][values]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    script->on_evaluation = [script](std::string_view text) {
        auto v = parsed_values(text, {0, 3, 0, 9});
        script->reference_output = v.size() == 3 && (v[0] == 4 || v[0] == 0) && v[1] == 4 && v[2] == 9 ? "2" : "1";
    };
    Runner runner(std::move(backend));
    auto input = request(); input.input_text = "3 4 4 9"; input.constraints = {0, 3, 0, 9}; input.limits.max_evaluations.reset();
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.outcome == RunOutcome::completed);
    REQUIRE(observed_values(result.best->normalized_input) == std::vector<std::int64_t>{0, 4, 9});
}
namespace {
using Values = std::vector<std::int64_t>;
using Predicate = std::function<CandidateOutcome(const Values&)>;
auto value_rank(std::int64_t value) {
    return std::pair{value < 0 ? -static_cast<__int128>(value) : static_cast<__int128>(value), value < 0};
}
ReductionResult scripted_reduction(ReductionRequest input, const Predicate& predicate,
                                   std::vector<std::string>* accepted = nullptr, const detail::CacheControls& cache = {}) {
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    const auto constraints = input.constraints;
    script->on_evaluation = [script, constraints, predicate](std::string_view text) {
        const auto outcome = predicate(parsed_values(text, constraints));
        script->reference_output = outcome == CandidateOutcome::lost ? "1" : "2";
        script->exit_codes[1] = outcome == CandidateOutcome::inconclusive ? 7 : 0;
    };
    Runner runner(std::move(backend));
    auto previous = observed_values(input.input_text);
    RunHooks hooks; hooks.notify = [&](const RunEvent& event) {
        REQUIRE(serialize_input(event.best.input) == event.best.normalized_input);
        if (event.kind != RunEventKind::acceptance) return;
        REQUIRE(std::holds_alternative<ArrayNode>(parse_input(event.best.normalized_input, constraints)));
        const auto current = observed_values(event.best.normalized_input);
        REQUIRE(predicate(current) == CandidateOutcome::preserved);
        // Independent ranking oracle uses a wider signed integer.
        REQUIRE((current.size() < previous.size() ||
            (current.size() == previous.size() && std::lexicographical_compare(
                current.begin(), current.end(), previous.begin(), previous.end(),
                [&](auto a, auto b) { return value_rank(a) < value_rank(b); }))));
        previous = current;
        if (accepted) accepted->push_back(event.best.normalized_input);
    };
    return reduce(std::move(input), runner, {}, hooks, cache);
}
// Enumerate every promised direct local move on the exact result. This oracle
// does not follow the search's chunk traversal, visitation or binary heuristic.
void check_local_guarantee(const ReductionResult& result, const InputConstraints& constraints,
                           std::int64_t simplest, const Predicate& predicate) {
    REQUIRE(result.statistics.round == RoundStatus::complete);
    const auto original = observed_values(result.best->normalized_input);
    std::set<std::string> inconclusive;
    auto check = [&](const Values& candidate) {
        const auto text = oracle_text(candidate);
        if (std::holds_alternative<ValidationError>(parse_input(text, constraints))) return;
        if (candidate == original) return;
        if (candidate.size() == original.size() && !std::lexicographical_compare(
            candidate.begin(), candidate.end(), original.begin(), original.end(),
            [&](auto a, auto b) { return value_rank(a) < value_rank(b); })) return;
        REQUIRE(predicate(candidate) != CandidateOutcome::preserved);
        if (predicate(candidate) == CandidateOutcome::inconclusive) inconclusive.insert(text);
    };
    for (std::size_t index = 0; index < original.size(); ++index) {
        auto removed = original; removed.erase(removed.begin() + index); check(removed);
        const auto target = original[index];
        Values replacements{simplest};
        if (target != 0) replacements.push_back(target < 0 ? target + 1 : target - 1);
        if (target < 0 && target != std::numeric_limits<std::int64_t>::min()) replacements.push_back(-target);
        for (auto replacement : replacements) {
            auto group = original;
            for (auto& value : group) if (value == target) value = replacement;
            check(group);
            if (std::count(original.begin(), original.end(), target) > 1) {
                auto individual = original; individual[index] = replacement; check(individual);
            }
        }
    }
    // The final round may also try larger removal chunks; every inconclusive
    // direct local check must be counted, and duplicate texts count only once.
    REQUIRE(result.statistics.distinct_inconclusive_candidates >= inconclusive.size());
}
}
TEST_CASE("signed extremes and constrained simplest values reduce safely", "[reduction][values]") {
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    for (auto original : {low, high}) {
        auto input = request(); input.input_text = "1 " + std::to_string(original);
        input.constraints = {1, 1, low, high}; input.limits.max_evaluations.reset();
        Predicate preserves = [](const Values& v) { return v[0] >= 3 || v[0] <= -3 ? CandidateOutcome::preserved : CandidateOutcome::lost; };
        auto result = scripted_reduction(input, preserves);
        REQUIRE(result.outcome == RunOutcome::completed);
        REQUIRE(observed_values(result.best->normalized_input) == Values{3});
        check_local_guarantee(result, input.constraints, 0, preserves);
    }
    for (const auto bounds : {std::pair{-10, -3}, std::pair{3, 10}}) {
        auto input = request(); input.constraints = {1, 1, bounds.first, bounds.second};
        input.input_text = "1 " + std::to_string(bounds.first < 0 ? bounds.first : bounds.second);
        input.limits.max_evaluations.reset();
        Predicate preserves = [](const Values&) { return CandidateOutcome::preserved; };
        auto result = scripted_reduction(input, preserves);
        const auto expected = bounds.first < 0 ? bounds.second : bounds.first;
        REQUIRE(observed_values(result.best->normalized_input) == Values{expected});
        check_local_guarantee(result, input.constraints, expected, preserves);
    }
    auto input = request(); input.input_text = "1 " + std::to_string(low);
    input.constraints = {1, 1, low, high}; input.limits.max_evaluations.reset();
    Predicate only_min = [](const Values& v) { return v[0] == low ? CandidateOutcome::preserved : CandidateOutcome::lost; };
    auto result = scripted_reduction(input, only_min);
    REQUIRE(observed_values(result.best->normalized_input) == Values{low});
    REQUIRE(result.statistics.candidate_evaluations == 2);
    check_local_guarantee(result, input.constraints, 0, only_min);
}
TEST_CASE("value simplification enables removal in the next round and is repeatable", "[reduction][values]") {
    auto input = request(); input.input_text = "2 5 5"; input.constraints = {0, 2, 0, 5}; input.limits.max_evaluations.reset();
    Predicate preserves = [](const Values& v) {
        return (v.size() == 2 && v[0] == v[1]) || (v.size() == 1 && v[0] == 0)
            ? CandidateOutcome::preserved : CandidateOutcome::lost;
    };
    std::vector<std::string> first, second;
    auto result = scripted_reduction(input, preserves, &first);
    auto repeated = scripted_reduction(input, preserves, &second);
    REQUIRE(result.best->normalized_input == "1\n0\n");
    REQUIRE(first == std::vector<std::string>{"2\n0 0\n", "1\n0\n"});
    REQUIRE(second == first);
    REQUIRE(repeated.best->normalized_input == result.best->normalized_input);
    check_local_guarantee(result, input.constraints, 0, preserves);
}
TEST_CASE("non-threshold failures establish only the direct local guarantee", "[reduction][values]") {
    auto input = request(); input.input_text = "1 100"; input.constraints = {1, 1, 0, 100}; input.limits.max_evaluations.reset();
    Predicate preserves = [](const Values& v) {
        return v[0] == 100 || v[0] == 99 || v[0] == 50 ? CandidateOutcome::preserved : CandidateOutcome::lost;
    };
    auto result = scripted_reduction(input, preserves);
    REQUIRE(observed_values(result.best->normalized_input) == Values{99});
    REQUIRE(preserves({50}) == CandidateOutcome::preserved);
    check_local_guarantee(result, input.constraints, 0, preserves);
}
TEST_CASE("ordered unique candidates are validated before execution", "[reduction][values]") {
    for (bool ascending : {true, false}) {
        auto input = request(); input.input_text = ascending ? "3 -3 0 3" : "3 3 0 -3";
        input.constraints = {3, 3, -3, 3, ascending ? Ordering::ascending : Ordering::descending, true};
        input.limits.max_evaluations.reset();
        Predicate preserves = [](const Values&) { return CandidateOutcome::preserved; };
        auto result = scripted_reduction(input, preserves);
        REQUIRE(observed_values(result.best->normalized_input) == (ascending ? Values{-1, 0, 1} : Values{1, 0, -1}));
        check_local_guarantee(result, input.constraints, 0, preserves);
    }
}
TEST_CASE("complete and partial inconclusive counts deduplicate texts and reset each round", "[reduction][values]") {
    auto input = request(); input.input_text = "2 5 5"; input.constraints = {0, 2, 0, 5}; input.limits.max_evaluations.reset();
    Predicate only_original = [](const Values& v) { return v == Values{5, 5} ? CandidateOutcome::preserved : CandidateOutcome::inconclusive; };
    auto result = scripted_reduction(input, only_original);
    REQUIRE(result.outcome == RunOutcome::completed);
    REQUIRE(result.statistics.inconclusive_executions == 7);
    REQUIRE(result.statistics.distinct_inconclusive_candidates == 7);
    check_local_guarantee(result, input.constraints, 0, only_original);
    input.limits.max_evaluations = 2;
    auto partial = scripted_reduction(input, only_original);
    REQUIRE(partial.outcome == RunOutcome::evaluation_limit);
    REQUIRE(partial.statistics.round == RoundStatus::partial);
    REQUIRE(partial.statistics.inconclusive_executions == 2);
    REQUIRE(partial.statistics.distinct_inconclusive_candidates == 2);
    input.limits.max_evaluations.reset();
    Predicate equal_pair = [](const Values& v) { return v.size() == 2 && v[0] == v[1] ? CandidateOutcome::preserved : CandidateOutcome::inconclusive; };
    auto simplified = scripted_reduction(input, equal_pair);
    REQUIRE(observed_values(simplified.best->normalized_input) == Values{0, 0});
    REQUIRE(simplified.statistics.inconclusive_executions == 2);
    REQUIRE(simplified.statistics.distinct_inconclusive_candidates == 1);
    check_local_guarantee(simplified, input.constraints, 0, equal_pair);
}
TEST_CASE("exact evaluation limits distinguish completed uncertainty from an unfinished round", "[reduction][values]") {
    auto input = request(); input.input_text = "2 5 5"; input.constraints = {0, 2, 0, 5};
    Predicate only_original = [](const Values& v) { return v == Values{5, 5} ? CandidateOutcome::preserved : CandidateOutcome::inconclusive; };
    input.limits.max_evaluations = 7;
    auto complete = scripted_reduction(input, only_original);
    REQUIRE(complete.outcome == RunOutcome::completed);
    REQUIRE(complete.observed_reasons.evaluation_limit);
    REQUIRE(complete.statistics.round == RoundStatus::complete);
    REQUIRE(complete.statistics.candidate_evaluations == 7);
    REQUIRE(complete.best->evidence == complete.baseline.evaluation);
    input.limits.max_evaluations = 6;
    auto partial = scripted_reduction(input, only_original);
    REQUIRE(partial.outcome == RunOutcome::evaluation_limit);
    REQUIRE(partial.statistics.round == RoundStatus::partial);
    REQUIRE(partial.statistics.candidate_evaluations == 6);
}
TEST_CASE("cancellation during value evaluation and acceptance preserves verified recovery", "[reduction][values]") {
    for (bool during_evaluation : {true, false}) {
        auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
        CancellationSource source;
        script->on_evaluation = [&](std::string_view text) {
            if (during_evaluation && text != "1\n5\n") source.request_stop();
        };
        Runner runner(std::move(backend));
        auto input = request(); input.input_text = "1 5"; input.constraints = {1, 1, 0, 5}; input.limits.max_evaluations.reset();
        std::vector<std::string> saved;
        RunHooks hooks;
        hooks.notify = [&](const RunEvent& event) { if (event.kind == RunEventKind::acceptance) source.request_stop(); };
        hooks.save = [&](const SaveRequest& save) -> SaveResult { saved.emplace_back(save.normalized_input); return {}; };
        auto result = reduce(std::move(input), runner, source.token(), hooks);
        REQUIRE(result.outcome == RunOutcome::cancelled);
        REQUIRE(result.statistics.round == RoundStatus::partial);
        REQUIRE(result.statistics.acceptances == (during_evaluation ? 0 : 1));
        REQUIRE(result.best->normalized_input == (during_evaluation ? "1\n5\n" : "1\n0\n"));
        REQUIRE(saved.size() == 2);
        REQUIRE(saved.back() == result.best->normalized_input);
    }
}
TEST_CASE("final save errors preserve completed search facts and verified evidence", "[reduction][values]") {
    Runner runner(std::make_unique<BaselineBackend>());
    auto input = request(); input.input_text = "1 5"; input.constraints = {1, 1, 0, 5}; input.limits.max_evaluations.reset();
    RunHooks hooks; hooks.save = [](const SaveRequest& save) -> SaveResult {
        if (save.kind == SaveKind::final) throw std::runtime_error("final save");
        return {};
    };
    auto result = reduce(std::move(input), runner, {}, hooks);
    REQUIRE(result.outcome == RunOutcome::completed);
    REQUIRE(result.statistics.round == RoundStatus::complete);
    REQUIRE(observed_values(result.best->normalized_input) == Values{0});
    REQUIRE(result.best->evidence->outcome == CandidateOutcome::preserved);
    REQUIRE(result.saving.final_outcome == FinalSaveOutcome::failed);
    REQUIRE(result.saving.last_saved_revision == 1);
    REQUIRE(result.errors[0].kind == RunErrorKind::save);
}
TEST_CASE("repeated inconclusive candidate texts execute once without changing complete-round counts", "[reduction][cache]") {
    auto input = request(); input.input_text = "2 5 5"; input.constraints = {0, 2, 0, 5}; input.limits.max_evaluations = 7;
    Predicate only_original = [](const Values& v) { return v == Values{5, 5} ? CandidateOutcome::preserved : CandidateOutcome::inconclusive; };
    auto result = scripted_reduction(input, only_original);
    REQUIRE(result.outcome == RunOutcome::completed);
    REQUIRE(result.statistics.candidate_evaluations == 7);
    REQUIRE(result.statistics.inconclusive_executions == 7);
    REQUIRE(result.statistics.distinct_inconclusive_candidates == 7);
    REQUIRE(result.statistics.round == RoundStatus::complete);
    REQUIRE(result.best->evidence == result.baseline.evaluation);
}
TEST_CASE("forced fingerprint collisions cannot skip preserving candidates or change completed sequences", "[reduction][cache]") {
    auto input = request(); input.input_text = "3 42 7 42"; input.constraints = {0, 3, 0, 42}; input.limits.max_evaluations.reset();
    Predicate duplicates = [](const Values& values) {
        return std::set<std::int64_t>(values.begin(), values.end()).size() < values.size()
            ? CandidateOutcome::preserved : CandidateOutcome::lost;
    };
    std::vector<std::string> expected;
    auto uncached = scripted_reduction(input, duplicates, &expected, {.enabled = false});
    REQUIRE(expected == std::vector<std::string>{"2\n42 42\n", "2\n0 0\n"});
    for (const auto& controls : std::vector<detail::CacheControls>{
        {}, {.forced_fingerprint = 0}, {.max_entries = 1, .forced_fingerprint = 0},
        {.max_text_bytes = 5, .forced_fingerprint = 0}, {.max_entries = 0}, {.max_text_bytes = 0}}) {
        std::vector<std::string> accepted;
        auto result = scripted_reduction(input, duplicates, &accepted, controls);
        REQUIRE(result.outcome == RunOutcome::completed);
        REQUIRE(result.best->normalized_input == uncached.best->normalized_input);
        REQUIRE(accepted == expected);
        REQUIRE(result.statistics.distinct_inconclusive_candidates == uncached.statistics.distinct_inconclusive_candidates);
        REQUIRE(result.statistics.candidate_evaluations <= uncached.statistics.candidate_evaluations);
        check_local_guarantee(result, input.constraints, 0, duplicates);
    }
}
TEST_CASE("cached inconclusive outcomes remain distinct by exact text despite collisions eviction and rounds", "[reduction][cache]") {
    auto input = request(); input.input_text = "2 5 5"; input.constraints = {0, 2, 0, 5}; input.limits.max_evaluations.reset();
    Predicate only_original = [](const Values& v) { return v == Values{5, 5} ? CandidateOutcome::preserved : CandidateOutcome::inconclusive; };
    auto uncached = scripted_reduction(input, only_original, nullptr, {.enabled = false});
    REQUIRE(uncached.statistics.inconclusive_executions == 8);
    for (const auto& controls : std::vector<detail::CacheControls>{
        {}, {.forced_fingerprint = 1}, {.max_entries = 1, .forced_fingerprint = 1}, {.max_text_bytes = 5}}) {
        auto result = scripted_reduction(input, only_original, nullptr, controls);
        REQUIRE(result.outcome == RunOutcome::completed);
        REQUIRE(result.best->normalized_input == uncached.best->normalized_input);
        REQUIRE(result.statistics.inconclusive_executions == 7);
        REQUIRE(result.statistics.distinct_inconclusive_candidates == 7);
    }
    Predicate equal_pair = [](const Values& v) { return v.size() == 2 && v[0] == v[1] ? CandidateOutcome::preserved : CandidateOutcome::inconclusive; };
    auto cached = scripted_reduction(input, equal_pair, nullptr, {.forced_fingerprint = 0});
    auto plain = scripted_reduction(input, equal_pair, nullptr, {.enabled = false});
    REQUIRE(observed_values(cached.best->normalized_input) == Values{0, 0});
    REQUIRE(cached.best->normalized_input == plain.best->normalized_input);
    REQUIRE(cached.statistics.inconclusive_executions == 2);
    REQUIRE(plain.statistics.inconclusive_executions == 4);
    REQUIRE(cached.statistics.distinct_inconclusive_candidates == 1);
    REQUIRE(plain.statistics.distinct_inconclusive_candidates == 1);
}
TEST_CASE("lost outcomes save executions while cache capacity changes only work performed", "[reduction][cache]") {
    auto input = request(); input.input_text = "2 5 5"; input.constraints = {0, 2, 0, 5}; input.limits.max_evaluations.reset();
    Predicate pair = [](const Values& v) { return v.size() == 2 && v[0] == v[1] ? CandidateOutcome::preserved : CandidateOutcome::lost; };
    std::vector<std::string> expected;
    auto plain = scripted_reduction(input, pair, &expected, {.enabled = false});
    auto cached = scripted_reduction(input, pair);
    auto evicted = scripted_reduction(input, pair, nullptr, {.max_entries = 1, .max_text_bytes = 4, .forced_fingerprint = 0});
    REQUIRE(cached.statistics.lost_executions < plain.statistics.lost_executions);
    REQUIRE(cached.best->normalized_input == plain.best->normalized_input);
    REQUIRE(evicted.best->normalized_input == plain.best->normalized_input);
    REQUIRE(cached.statistics.acceptances == plain.statistics.acceptances);
    REQUIRE(cached.statistics.distinct_inconclusive_candidates == 0);
    REQUIRE(evicted.outcome == RunOutcome::completed);
}
TEST_CASE("errors cancelled evaluations and prior invocation cache state are never reused", "[reduction][cache]") {
    for (bool failure : {true, false}) {
        auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
        CancellationSource source;
        bool first_run = true;
        std::size_t baseline_launches = 0, candidate_launches = 0;
        script->on_evaluation = [&](std::string_view text) {
            if (text == "1\n5\n") { ++baseline_launches; return; }
            ++candidate_launches;
            if (!first_run) return;
            if (failure) script->failure = RunnerError{RunnerErrorCode::io, {}, EIO, "candidate failure"};
            else source.request_stop();
        };
        Runner runner(std::move(backend));
        auto input = request(); input.input_text = "1 5"; input.constraints = {1, 1, 0, 5}; input.limits.max_evaluations.reset();
        auto interrupted = reduce(input, runner, source.token(), {}, {.forced_fingerprint = 0});
        REQUIRE(interrupted.outcome == (failure ? RunOutcome::operational_stop : RunOutcome::cancelled));
        REQUIRE(observed_values(interrupted.best->normalized_input) == Values{5});
        REQUIRE(interrupted.statistics.acceptances == 0);
        first_run = false; script->failure.reset();
        auto fresh = reduce(input, runner, {}, {}, {.forced_fingerprint = 0});
        REQUIRE(fresh.outcome == RunOutcome::completed);
        REQUIRE(observed_values(fresh.best->normalized_input) == Values{0});
        REQUIRE(baseline_launches == 2);
        REQUIRE(candidate_launches == 2);
    }
    auto backend = std::make_unique<BaselineBackend>(); auto* script = backend.get();
    unsigned candidate_launches = 0;
    script->on_evaluation = [&](std::string_view text) {
        if (text == "1\n1\n") { script->reference_output = "2"; return; }
        ++candidate_launches; script->reference_output = "1";
    };
    Runner runner(std::move(backend));
    auto input = request(); input.input_text = "1 1"; input.constraints = {1, 1, 0, 1}; input.limits.max_evaluations.reset();
    for (int invocation = 0; invocation < 2; ++invocation) {
        auto result = reduce(input, runner);
        REQUIRE(result.outcome == RunOutcome::completed);
        REQUIRE(result.statistics.candidate_evaluations == 1);
        REQUIRE(result.statistics.lost_executions == 1);
    }
    REQUIRE(candidate_launches == 2);
}
TEST_CASE("cache hits after the last allowed execution can establish completion", "[reduction][cache]") {
    auto input = request(); input.input_text = "1 1"; input.constraints = {1, 1, 0, 1}; input.limits.max_evaluations = 1;
    Predicate only_original = [](const Values& v) { return v[0] == 1 ? CandidateOutcome::preserved : CandidateOutcome::inconclusive; };
    auto result = scripted_reduction(input, only_original);
    REQUIRE(result.outcome == RunOutcome::completed);
    REQUIRE(result.observed_reasons.evaluation_limit);
    REQUIRE(result.statistics.candidate_evaluations == 1);
    REQUIRE(result.statistics.inconclusive_executions == 1);
    REQUIRE(result.statistics.distinct_inconclusive_candidates == 1);
    auto no_cache = scripted_reduction(input, only_original, nullptr, {.enabled = false});
    REQUIRE(no_cache.outcome == RunOutcome::evaluation_limit);
    REQUIRE(no_cache.statistics.round == RoundStatus::partial);
}
TEST_CASE("eviction reexecutes old texts while completed binary-search sequences remain identical", "[reduction][cache]") {
    auto input = request(); input.input_text = "1 100"; input.constraints = {1, 1, 0, 100}; input.limits.max_evaluations.reset();
    Predicate threshold = [](const Values& v) { return v[0] >= 37 ? CandidateOutcome::preserved : CandidateOutcome::lost; };
    std::vector<std::string> plain_sequence, cached_sequence, evicted_sequence;
    auto plain = scripted_reduction(input, threshold, &plain_sequence, {.enabled = false});
    auto cached = scripted_reduction(input, threshold, &cached_sequence, {.forced_fingerprint = 0});
    auto evicted = scripted_reduction(input, threshold, &evicted_sequence, {.max_entries = 1, .forced_fingerprint = 0});
    REQUIRE(cached.outcome == RunOutcome::completed);
    REQUIRE(evicted.outcome == RunOutcome::completed);
    REQUIRE(observed_values(cached.best->normalized_input) == Values{37});
    REQUIRE(evicted.best->normalized_input == plain.best->normalized_input);
    REQUIRE(cached_sequence == plain_sequence);
    REQUIRE(evicted_sequence == plain_sequence);
    REQUIRE(cached.statistics.candidate_evaluations < plain.statistics.candidate_evaluations);
    REQUIRE(evicted.statistics.candidate_evaluations > cached.statistics.candidate_evaluations);
}
TEST_CASE("real compiled programs preserve completed accepted sequences across cache controls", "[reduction][cache][runner]") {
    auto input = request(); input.input_text = "3 42 7 42"; input.constraints = {0, 3, 0, 42}; input.limits.max_evaluations.reset();
    input.solution = {SHRINK_PROGRAM_FIXTURE, {"duplicate-bug"}};
    input.reference = {SHRINK_PROGRAM_FIXTURE, {"text", "ok"}};
    for (const auto& controls : std::vector<detail::CacheControls>{
        {.enabled = false}, {}, {.forced_fingerprint = 0}, {.max_entries = 1, .forced_fingerprint = 0}, {.max_text_bytes = 4}}) {
        Runner runner;
        std::vector<std::string> accepted;
        RunHooks hooks; hooks.notify = [&](const RunEvent& event) {
            if (event.kind == RunEventKind::acceptance) accepted.push_back(event.best.normalized_input);
        };
        auto result = reduce(input, runner, {}, hooks, controls);
        REQUIRE(result.outcome == RunOutcome::completed);
        REQUIRE(observed_values(result.best->normalized_input) == Values{0, 0});
        REQUIRE(accepted == std::vector<std::string>{"2\n42 42\n", "2\n0 0\n"});
        REQUIRE(result.statistics.distinct_inconclusive_candidates == 0);
    }
}
TEST_CASE("final-round uncertainty includes cache hits learned before an acceptance", "[reduction][cache]") {
    auto input = request(); input.input_text = "2 5 5"; input.constraints = {0, 2, 0, 5}; input.limits.max_evaluations.reset();
    Predicate preserves = [](const Values& v) {
        return v == Values{5, 5} || v == Values{0, 5} ? CandidateOutcome::preserved : CandidateOutcome::inconclusive;
    };
    std::vector<std::string> accepted, plain_accepted;
    auto cached = scripted_reduction(input, preserves, &accepted, {.forced_fingerprint = 0});
    auto plain = scripted_reduction(input, preserves, &plain_accepted, {.enabled = false});
    REQUIRE(cached.outcome == RunOutcome::completed);
    REQUIRE(observed_values(cached.best->normalized_input) == Values{0, 5});
    REQUIRE(accepted == plain_accepted);
    REQUIRE(cached.statistics.candidate_evaluations == 6);
    REQUIRE(plain.statistics.candidate_evaluations == 9);
    REQUIRE(cached.statistics.inconclusive_executions == 5);
    REQUIRE(cached.statistics.distinct_inconclusive_candidates == 4);
    REQUIRE(plain.statistics.distinct_inconclusive_candidates == 4);
    check_local_guarantee(cached, input.constraints, 0, preserves);
}
TEST_CASE("overall deadline exact boundary distinguishes verified from unfinished baselines", "[reduction][time-limit]") {
    for (std::uint64_t delay : {49, 50}) {
        auto backend = std::make_unique<BaselineBackend>(); backend->service_delay = delay;
        Runner runner(std::move(backend));
        auto input = request(); input.limits.time_limit = std::chrono::milliseconds(50);
        std::vector<SaveKind> saves;
        RunHooks hooks; hooks.save = [&](const SaveRequest& save) -> SaveResult { saves.push_back(save.kind); return {}; };
        auto result = reduce(std::move(input), runner, {}, hooks);
        REQUIRE(result.statistics.elapsed_ms == delay);
        REQUIRE_FALSE(result.observed_reasons.cancellation);
        REQUIRE(result.observed_reasons.time_limit == (delay == 50));
        if (delay == 49) {
            REQUIRE(result.best);
            REQUIRE(result.outcome == RunOutcome::evaluation_limit);
            REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::final});
        } else {
            REQUIRE_FALSE(result.best);
            REQUIRE(result.outcome == RunOutcome::time_limit);
            REQUIRE(result.baseline.status == BaselineStatus::interrupted);
            REQUIRE(result.statistics.round == RoundStatus::not_started);
            REQUIRE(saves.empty());
        }
    }
}
TEST_CASE("sleep-inclusive search expiry discards unfinished work and no overall budget is the default", "[reduction][time-limit]") {
    for (bool limited : {true, false}) {
        auto backend = std::make_unique<BaselineBackend>(); auto* clock = backend.get();
        clock->on_evaluation = [clock](std::string_view text) { if (!text.starts_with("3\n")) clock->service_delay = 3'600'000; };
        Runner runner(std::move(backend));
        auto input = request(); input.limits.max_evaluations = 1;
        input.limits.solution_timeout = input.limits.reference_timeout = std::chrono::hours(2);
        if (limited) input.limits.time_limit = std::chrono::seconds(1);
        std::vector<std::uint64_t> saved;
        RunHooks hooks; hooks.save = [&](const SaveRequest& save) -> SaveResult { saved.push_back(save.revision); return {}; };
        auto result = reduce(std::move(input), runner, {}, hooks);
        REQUIRE(result.outcome == (limited ? RunOutcome::time_limit : RunOutcome::evaluation_limit));
        REQUIRE(result.observed_reasons.time_limit == limited);
        REQUIRE_FALSE(result.observed_reasons.cancellation);
        REQUIRE(result.statistics.candidate_evaluations == 1);
        REQUIRE(result.statistics.acceptances == (limited ? 0 : 1));
        REQUIRE(result.best->revision == (limited ? 1 : 2));
        REQUIRE(result.statistics.elapsed_ms == 3'600'000);
        REQUIRE(result.statistics.round == RoundStatus::partial);
        REQUIRE(result.pending_ownership.empty());
        REQUIRE(result.errors.empty());
        REQUIRE(saved.front() == 1);
        REQUIRE(saved.back() == result.best->revision);
    }
}
TEST_CASE("checkpoint expiry preserves the accepted best and all error and cancellation observations", "[reduction][time-limit]") {
    for (bool fail : {false, true}) {
        auto backend = std::make_unique<BaselineBackend>(); auto* clock = backend.get();
        clock->service_delay = 600;
        Runner runner(std::move(backend));
        auto input = request(); input.limits.max_evaluations.reset(); input.limits.time_limit = std::chrono::milliseconds(3000);
        CancellationSource source;
        std::vector<SaveKind> saves;
        RunHooks hooks;
        hooks.save = [&](const SaveRequest& save) -> SaveResult {
            saves.push_back(save.kind);
            if (save.kind == SaveKind::periodic) {
                clock->clock += 1200;
                if (fail) { source.request_stop(); throw std::runtime_error("checkpoint failed at deadline"); }
            }
            if (save.kind == SaveKind::final) clock->clock += 500;
            return {};
        };
        auto result = reduce(std::move(input), runner, source.token(), hooks);
        REQUIRE(result.outcome == (fail ? RunOutcome::operational_stop : RunOutcome::time_limit));
        REQUIRE(result.observed_reasons.time_limit);
        REQUIRE(result.observed_reasons.cancellation == fail);
        REQUIRE(result.statistics.candidate_evaluations == 2);
        REQUIRE(result.statistics.acceptances == 2);
        REQUIRE(result.best->normalized_input == "1\n7\n");
        REQUIRE(result.statistics.round == RoundStatus::partial);
        REQUIRE(result.statistics.elapsed_ms == 3500);
        REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::periodic, SaveKind::final});
        REQUIRE(result.saving.last_saved_revision == 3);
        REQUIRE(result.saving.final_outcome == FinalSaveOutcome::succeeded);
        REQUIRE(result.errors.size() == (fail ? 1 : 0));
    }
}
TEST_CASE("completed search facts survive final saving that runs beyond the overall deadline", "[reduction][time-limit]") {
    for (bool fail : {false, true}) {
        auto backend = std::make_unique<BaselineBackend>(); auto* clock = backend.get();
        Runner runner(std::move(backend));
        auto input = request(); input.input_text = "1 0"; input.constraints = {1, 1, 0, 0};
        input.limits.max_evaluations.reset(); input.limits.time_limit = std::chrono::milliseconds(50);
        RunHooks hooks;
        hooks.save = [&](const SaveRequest& save) -> SaveResult {
            if (save.kind == SaveKind::final) {
                clock->clock += 100;
                if (fail) throw std::runtime_error("final saving after completion");
            }
            return {};
        };
        auto result = reduce(std::move(input), runner, {}, hooks);
        REQUIRE(result.outcome == RunOutcome::completed);
        REQUIRE(result.statistics.round == RoundStatus::complete);
        REQUIRE(result.observed_reasons.time_limit);
        REQUIRE_FALSE(result.observed_reasons.cancellation);
        REQUIRE(result.statistics.elapsed_ms == 100);
        REQUIRE(result.best->evidence == result.baseline.evaluation);
        REQUIRE(result.saving.final_outcome == (fail ? FinalSaveOutcome::failed : FinalSaveOutcome::succeeded));
        REQUIRE(result.errors.size() == (fail ? 1 : 0));
    }
}
TEST_CASE("overall expiry checks candidate processing even when invalid or cached moves launch nothing", "[reduction][time-limit]") {
    for (bool invalid : {true, false}) {
        auto backend = std::make_unique<BaselineBackend>(); auto* clock = backend.get();
        bool processing = false;
        clock->on_clock = [clock, &processing] { if (processing) ++clock->clock; };
        clock->on_evaluation = [clock](std::string_view text) { if (!text.starts_with("128\n")) clock->reference_output = "1"; };
        Runner runner(std::move(backend));
        auto input = request(); input.input_text = "128";
        for (int index = 0; index < 128; ++index) input.input_text += " 0";
        input.constraints = {invalid ? 128 : 0, 128, 0, 0};
        input.limits.max_evaluations.reset(); input.limits.time_limit = std::chrono::milliseconds(1000);
        std::vector<SaveKind> saves;
        RunHooks hooks;
        hooks.notify = [&](const RunEvent& event) { if (event.kind == RunEventKind::baseline_verified) processing = true; };
        hooks.save = [&](const SaveRequest& save) -> SaveResult { saves.push_back(save.kind); return {}; };
        auto result = reduce(std::move(input), runner, {}, hooks);
        REQUIRE(result.outcome == RunOutcome::time_limit);
        REQUIRE(result.observed_reasons.time_limit);
        REQUIRE_FALSE(result.observed_reasons.cancellation);
        REQUIRE(result.statistics.round == RoundStatus::partial);
        REQUIRE(result.statistics.acceptances == 0);
        REQUIRE(result.statistics.candidate_evaluations <= 7);
        REQUIRE(result.statistics.candidate_evaluations == result.statistics.lost_executions);
        if (invalid) REQUIRE(result.statistics.candidate_evaluations == 0);
        else REQUIRE(result.statistics.candidate_evaluations > 0);
        REQUIRE(result.best->revision == 1);
        REQUIRE(saves == std::vector{SaveKind::initial, SaveKind::final});
    }
}
TEST_CASE("request validation finishes before the overall elapsed clock starts", "[reduction][time-limit]") {
    auto backend = std::make_unique<BaselineBackend>(); auto* clock = backend.get();
    unsigned clock_reads = 0; clock->on_clock = [&] { ++clock_reads; };
    Runner runner(std::move(backend));
    auto input = request(); input.input_text = "invalid"; input.limits.time_limit = std::chrono::milliseconds(1);
    auto result = reduce(std::move(input), runner);
    REQUIRE(result.outcome == RunOutcome::invalid_request);
    REQUIRE(clock_reads == 0);
    REQUIRE(clock->inputs.empty());
    REQUIRE_FALSE(result.best);
}
TEST_CASE("cleanup errors retain simultaneous caller cancellation and overall expiry before verification", "[reduction][time-limit]") {
    auto backend = std::make_unique<BaselineBackend>();
    CancellationSource source;
    backend->on_evaluation = [&](std::string_view) { source.request_stop(); };
    backend->service_delay = 50;
    backend->failure = RunnerError{RunnerErrorCode::cleanup, {}, EIO, "cleanup failed at expiry"};
    Runner runner(std::move(backend));
    auto input = request(); input.limits.time_limit = std::chrono::milliseconds(50);
    unsigned saves = 0;
    RunHooks hooks; hooks.save = [&](const SaveRequest&) -> SaveResult { ++saves; return {}; };
    auto result = reduce(std::move(input), runner, source.token(), hooks);
    REQUIRE(result.outcome == RunOutcome::operational_stop);
    REQUIRE(result.observed_reasons.cancellation);
    REQUIRE(result.observed_reasons.time_limit);
    REQUIRE(result.errors[0].kind == RunErrorKind::cleanup);
    REQUIRE_FALSE(result.best);
    REQUIRE(saves == 0);
    REQUIRE(result.statistics.round == RoundStatus::not_started);
}
