#include "evaluator.hpp"
#include "runner_backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <functional>
#include <exception>
#include <thread>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace shrink;
using namespace shrink::detail;
constexpr auto solution = ProgramRole::solution;
constexpr auto reference = ProgramRole::reference;
struct Step { std::uint64_t elapsed; std::vector<ExecutionEvent> events; };
class Script final : public ExecutionBackend {
public:
    std::uint64_t clock = 100;
    std::deque<Step> steps;
    std::array<bool, 2> active{};
    std::array<unsigned, 2> completion_parts{};
    std::array<bool, 2> uncertain_close{};
    std::function<void()> on_clock;
    std::function<void()> after_service;
    std::array<int, 2> stops{};
    std::array<std::optional<std::uint64_t>, 2> stopped_at{};
    std::array<int, 2> launches{};
    std::vector<int> waits;
    std::array<std::uint64_t, 2> launch_delay{};
    std::function<void(ProgramRole)> on_launch;
    std::optional<ProgramRole> fail_launch;
    std::optional<ProgramRole> fail_stop;
    std::uint64_t now_ms() override { if (on_clock) on_clock(); return clock; }
    std::optional<RunnerError> launch(ProgramRole p, const ProgramSpec&, std::string_view) override {
        ++launches[program_index(p)]; clock += launch_delay[program_index(p)];
        if (fail_launch == p) return RunnerError{RunnerErrorCode::launch, p, ENOENT, "scripted launch"};
        active[program_index(p)] = true;
        completion_parts[program_index(p)] = 0;
        if (on_launch) on_launch(p);
        return {};
    }
    bool pending(ProgramRole p) const override { return active[program_index(p)] || uncertain_close[program_index(p)]; }
    PendingOwnership ownership(ProgramRole p) const override {
        const auto parts = completion_parts[program_index(p)];
        const auto descriptors = static_cast<std::size_t>(!(parts & 1)) + !(parts & 2) + !(parts & 4);
        return {p, (p == solution ? 7000 : 7001), active[program_index(p)] && !(parts & 8), active[program_index(p)] ? descriptors : 0, uncertain_close[program_index(p)] ? 1u : 0u};
    }
    std::optional<RunnerError> stop(ProgramRole p) override {
        ++stops[program_index(p)]; stopped_at[program_index(p)] = clock;
        if (fail_stop == p) return RunnerError{RunnerErrorCode::cleanup, p, EPERM, "scripted SIGKILL failure"};
        return {};
    }
    std::vector<ExecutionEvent> service(CancellationToken, int wait_ms) override {
        waits.push_back(wait_ms);
        if (steps.empty()) throw std::runtime_error("script exhausted");
        auto step = std::move(steps.front()); steps.pop_front(); clock += step.elapsed;
        for (const auto& e : step.events) {
            if (!e.program) continue;
            auto& parts = completion_parts[program_index(*e.program)];
            if (e.kind == EventKind::setup_complete || (e.kind == EventKind::error && e.error_code == RunnerErrorCode::launch)) parts |= 1;
            if (e.kind == EventKind::stdout_closed) parts |= 2;
            if (e.kind == EventKind::stderr_closed) parts |= 4;
            if (e.kind == EventKind::exited || e.kind == EventKind::signalled) parts |= 8;
            if (parts == 15) active[program_index(*e.program)] = false;
        }
        if (after_service) after_service();
        return step.events;
    }
};
std::vector<ExecutionEvent> completion(ProgramRole program, int code) {
    return {{program, EventKind::setup_complete}, {program, EventKind::stdout_closed},
            {program, EventKind::stderr_closed}, {program, EventKind::exited, {}, code}};
}
}
TEST_CASE("solution failure stops its reference promptly in either completion order", "[execution-events]") {
    for (const auto first : program_roles) {
        auto script = std::make_unique<Script>(); auto* observed = script.get();
        script->steps = {{0, completion(first, 7)}, {0, completion((first == solution ? reference : solution), 9)}};
        Runner runner(std::move(script));
        auto result = evaluate_candidate(runner, "0\n\n", {"/solution", {}}, {"/reference", {}}, {});
        REQUIRE(result.outcome == CandidateOutcome::lost);
        REQUIRE(observed->stops[0] == 0);
        REQUIRE(observed->stops[1] == (first == solution ? 1 : 0));
    }
}
TEST_CASE("deadline covers collection and distinguishes pending setup from execution timeout", "[execution-events]") {
    for (bool setup_done : {false, true}) {
        auto script = std::make_unique<Script>();
        if (setup_done) script->steps.push_back({0, {{solution, EventKind::setup_complete}, {reference, EventKind::setup_complete}}});
        script->steps.push_back({10, completion(solution, 0)});
        script->steps.push_back({0, completion(reference, 0)});
        Runner runner(std::move(script));
        RunLimits limits; limits.solution_timeout = std::chrono::milliseconds(10);
        auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, limits);
        REQUIRE_FALSE(result.execution.programs[0].successful());
        if (setup_done) {
            REQUIRE(result.execution.errors.empty());
            REQUIRE(result.outcome == CandidateOutcome::lost);
            REQUIRE(result.execution.programs[0].stop_reasons == std::vector{StopReason::timeout});
        } else {
            REQUIRE_FALSE(result.execution.errors.empty());
            REQUIRE(result.execution.errors[0].code == RunnerErrorCode::launch);
            REQUIRE_FALSE(result.outcome);
        }
    }
}
TEST_CASE("invalid runner configuration is rejected before launching either program", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    Runner runner(std::move(script));
    RunLimits limits; limits.solution_timeout = std::chrono::milliseconds(0);
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, limits);
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.errors[0].code == RunnerErrorCode::configuration);
    REQUIRE_FALSE(result.execution.errors[0].program);
    REQUIRE_FALSE(observed->active[0]);
    REQUIRE_FALSE(observed->active[1]);
}
TEST_CASE("cancellation after only one launch stops that child and does not launch its peer", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](ProgramRole) { source.request_stop(); };
    script->steps = {{0, completion(solution, 0)}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    REQUIRE(result.execution.cancelled);
    REQUIRE_FALSE(result.outcome);
    REQUIRE(observed->launches == std::array{1, 0});
    REQUIRE(observed->stops == std::array{1, 0});
    REQUIRE(result.execution.programs[0].stop_reasons == std::vector{StopReason::cancellation});
    REQUIRE_FALSE(runner.has_pending_ownership());
}
TEST_CASE("setup includes launch work and expires before launching a peer", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->launch_delay[0] = 10;
    script->steps = {{0, completion(solution, 0)}};
    Runner runner(std::move(script));
    RunLimits limits; limits.solution_timeout = std::chrono::milliseconds(10);
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, limits);
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.errors[0].code == RunnerErrorCode::launch);
    REQUIRE(observed->launches == std::array{1, 0});
    REQUIRE(observed->stops == std::array{1, 0});
}
TEST_CASE("operational errors override ordinary failure and retain cancellation reasons", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](ProgramRole p) { if (p == reference) source.request_stop(); };
    auto failed = completion(solution, 7);
    failed.push_back({reference, EventKind::error, {}, EIO, RunnerErrorCode::io, "scripted read failure"});
    failed.push_back({std::nullopt, EventKind::error, {}, EIO, RunnerErrorCode::io, "scripted poll failure"});
    script->steps = {{0, failed}, {0, completion(reference, 0)}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.cancelled);
    REQUIRE(result.execution.errors[0].code == RunnerErrorCode::io);
    REQUIRE(result.execution.errors[0].program == reference);
    REQUIRE_FALSE(result.execution.errors[1].program);
    REQUIRE(result.execution.programs[0].exit_code == 7);
    REQUIRE(observed->stops == std::array{1, 1});
}
TEST_CASE("unresolved cleanup has one shared bound and retained ownership blocks new work", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](ProgramRole p) { if (p == reference) source.request_stop(); };
    script->steps = {{1000, {}}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    const bool pending = runner.has_pending_ownership();
    auto refused = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    const auto launches = observed->launches;
    const auto clock = observed->clock;
    // Deliver eventual OS completion through the controlled execution seam.
    // The production runner still owns its backend throughout the failed call.
    observed->steps = {{0, completion(solution, 0)}, {0, completion(reference, 0)}};
    observed->service({}, 0); observed->service({}, 0);
    REQUIRE(pending);
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.errors[0].code == RunnerErrorCode::cleanup);
    REQUIRE_FALSE(result.execution.programs[0].cleanup_complete);
    REQUIRE_FALSE(result.execution.programs[1].cleanup_complete);
    REQUIRE(refused.execution.errors[0].code == RunnerErrorCode::pending_ownership);
    REQUIRE(launches == std::array{1, 1});
    REQUIRE(clock == 1100);
    REQUIRE_FALSE(runner.has_pending_ownership());
}
TEST_CASE("cleanup waits never request time past the shared deadline", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](ProgramRole p) { if (p == reference) source.request_stop(); };
    auto both = completion(solution, 0); auto second = completion(reference, 0);
    both.insert(both.end(), second.begin(), second.end());
    script->steps = {{999, {}}, {1, both}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    REQUIRE(result.execution.errors.empty());
    REQUIRE(observed->waits == std::vector<int>{5, 1});
}
TEST_CASE("reference failure is inconclusive only when solution succeeds in either order", "[execution-events]") {
    for (const auto first : program_roles) {
        auto script = std::make_unique<Script>(); auto* observed = script.get();
        script->steps = {{0, completion(first, first == solution ? 0 : 127)},
                         {0, completion((first == solution ? reference : solution), first == solution ? 127 : 0)}};
        Runner runner(std::move(script));
        auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
        REQUIRE(result.execution.errors.empty());
        REQUIRE(result.outcome == CandidateOutcome::inconclusive);
        REQUIRE(observed->stops == std::array{0, 0});
    }
}
TEST_CASE("late operational errors override an already observed ordinary solution failure", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->steps = {{0, completion(solution, 7)},
        {0, {{reference, EventKind::error, {}, EACCES, RunnerErrorCode::launch, "exec"}}},
        {0, completion(reference, 127)}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.errors[0].code == RunnerErrorCode::launch);
    REQUIRE(observed->stops[1] == 1);
    REQUIRE(result.execution.programs[0].exit_code == 7);
}
TEST_CASE("sleep-inclusive clock jumps expire work before newly collected completion", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->steps = {{0, {{solution, EventKind::setup_complete}, {reference, EventKind::setup_complete}}},
                     {60'000, {}}, {0, completion(solution, 0)}, {0, completion(reference, 0)}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    REQUIRE(result.execution.errors.empty());
    REQUIRE(result.outcome == CandidateOutcome::lost);
    REQUIRE(observed->stops == std::array{1, 1});
    REQUIRE(result.execution.programs[0].stop_reasons[0] == StopReason::timeout);
    REQUIRE(result.execution.programs[1].stop_reasons[0] == StopReason::timeout);
}
TEST_CASE("completed reference cleanup does not shorten the solution execution deadline", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->steps = {{0, {{solution, EventKind::setup_complete}, {reference, EventKind::setup_complete}}},
                     {1, {}}, {5, completion(reference, 0)}, {2000, {}}, {0, completion(solution, 0)}};
    Runner runner(std::move(script));
    RunLimits limits; limits.reference_timeout = std::chrono::milliseconds(1);
    limits.solution_timeout = std::chrono::seconds(10);
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, limits);
    if (runner.has_pending_ownership()) observed->service({}, 0);
    REQUIRE(result.execution.errors.empty());
    REQUIRE(result.outcome == CandidateOutcome::inconclusive);
    REQUIRE(observed->stops == std::array{0, 1});
}
TEST_CASE("buffered output after exit must complete before wrong-answer acceptance", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->steps = {{0, {{solution, EventKind::setup_complete}, {reference, EventKind::setup_complete},
                         {solution, EventKind::stdout_data, "1"}, {reference, EventKind::stdout_data, "1.0"},
                         {solution, EventKind::exited, {}, 0}, {reference, EventKind::exited, {}, 0}}},
                     {0, {{solution, EventKind::stdout_data, ".0"}, {solution, EventKind::stdout_closed}, {solution, EventKind::stderr_closed},
                          {reference, EventKind::stdout_closed}, {reference, EventKind::stderr_closed}}}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    REQUIRE(result.outcome == CandidateOutcome::lost);
    REQUIRE(result.execution.errors.empty());
    REQUIRE(observed->steps.empty());
    REQUIRE(result.execution.programs[0].stdout_bytes == "1.0");
    REQUIRE(result.execution.programs[0].successful());
}
TEST_CASE("output collection remains deadline-bound after a zero exit", "[execution-events]") {
    auto script = std::make_unique<Script>();
    script->steps = {{0, {{solution, EventKind::setup_complete}, {reference, EventKind::setup_complete},
                         {solution, EventKind::exited, {}, 0}}},
                     {10, {{solution, EventKind::stdout_data, "a"}, {solution, EventKind::stdout_closed}, {solution, EventKind::stderr_closed}}},
                     {0, completion(reference, 0)}};
    Runner runner(std::move(script));
    RunLimits limits; limits.solution_timeout = std::chrono::milliseconds(10);
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, limits);
    REQUIRE(result.execution.errors.empty());
    REQUIRE(result.outcome == CandidateOutcome::lost);
    REQUIRE(result.execution.programs[0].exit_code == 0);
    REQUIRE(result.execution.programs[0].stop_reasons == std::vector{StopReason::timeout});
}
TEST_CASE("cancellation arriving after collection prevents output classification", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    std::vector<ExecutionEvent> both{{solution, EventKind::stdout_data, "1"}, {reference, EventKind::stdout_data, "1.0"}};
    auto first = completion(solution, 0); auto second = completion(reference, 0);
    both.insert(both.end(), first.begin(), first.end());
    both.insert(both.end(), second.begin(), second.end());
    script->steps = {{0, both}};
    CancellationSource source;
    int collection_clock_reads = 0;
    script->on_clock = [&] {
        if (observed->launches == std::array{1, 1} && !observed->pending(solution) && !observed->pending(reference)
            && ++collection_clock_reads == 2) source.request_stop();
    };
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    REQUIRE(source.token().stop_requested());
    REQUIRE(result.execution.cancelled);
    REQUIRE_FALSE(result.outcome);
    REQUIRE_FALSE(result.disagreement);
}
TEST_CASE("uncertain descriptor release retains ownership despite reaped zero exits", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->on_launch = [&](ProgramRole p) { if (p == solution) observed->uncertain_close[0] = true; };
    auto events = completion(solution, 0); auto reference_events = completion(reference, 0);
    events.insert(events.end(), reference_events.begin(), reference_events.end());
    events.push_back({solution, EventKind::error, {}, EINTR, RunnerErrorCode::cleanup, "descriptor release uncertain"});
    script->steps = {{0, events}, {1000, {}}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    const bool pending = runner.has_pending_ownership();
    auto refused = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    observed->uncertain_close[0] = false; // Simulated external confirmation of release.
    REQUIRE(pending);
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.errors[0].code == RunnerErrorCode::cleanup);
    REQUIRE(result.execution.programs[0].reaped);
    REQUIRE_FALSE(result.execution.programs[0].cleanup_complete);
    REQUIRE(refused.execution.errors[0].code == RunnerErrorCode::pending_ownership);
    REQUIRE(observed->launches == std::array{1, 1});
}
TEST_CASE("failed solution stops the reference before solution output EOF", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->steps = {{0, {{solution, EventKind::setup_complete}, {reference, EventKind::setup_complete},
                         {solution, EventKind::exited, {}, 7}}},
                     {100, {{solution, EventKind::stdout_closed}, {solution, EventKind::stderr_closed}}},
                     {0, completion(reference, 0)}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    REQUIRE(result.outcome == CandidateOutcome::lost);
    REQUIRE(observed->stopped_at[1] == 100);
    REQUIRE(observed->stops[0] == 0);
    REQUIRE(result.execution.programs[0].output_complete);
}
TEST_CASE("unexpected status errors retain children through termination and reaping", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->steps = {{0, {{solution, EventKind::setup_complete}, {reference, EventKind::setup_complete},
                         {solution, EventKind::error, {}, EIO, RunnerErrorCode::status, "unexpected nonterminal child status"}}},
                     {0, completion(solution, 0)}, {0, completion(reference, 0)}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.errors[0].code == RunnerErrorCode::status);
    REQUIRE(observed->stops == std::array{1, 1});
    REQUIRE(result.execution.programs[0].reaped);
    REQUIRE(result.execution.programs[1].reaped);
    REQUIRE_FALSE(runner.has_pending_ownership());
}
TEST_CASE("bounded recovery resolves retained children without changing the original error", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](ProgramRole p) { if (p == reference) source.request_stop(); };
    script->steps = {{0, {{solution, EventKind::stdout_data, "retained"}}}, {1000, {}}};
    Runner runner(std::move(script));
    auto original = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    const auto pending = runner.pending_ownership();
    observed->steps = {{0, completion(solution, 0)}, {0, completion(reference, 0)}};
    auto recovered = runner.cleanup();
    REQUIRE(pending.size() == 2);
    REQUIRE(pending[0].program == solution);
    REQUIRE(pending[1].program == reference);
    REQUIRE(recovered.resolved);
    REQUIRE(recovered.errors.empty());
    REQUIRE(recovered.execution.programs[0].reaped);
    REQUIRE(recovered.execution.programs[1].cleanup_complete);
    REQUIRE_FALSE(runner.has_pending_ownership());
    REQUIRE_FALSE(original.outcome);
    REQUIRE(original.execution.errors[0].code == RunnerErrorCode::cleanup);
    REQUIRE_FALSE(original.execution.programs[0].cleanup_complete);
    REQUIRE(recovered.execution.errors[0].code == RunnerErrorCode::cleanup);
    REQUIRE(recovered.execution.programs[0].stdout_bytes == "retained");
    REQUIRE(observed->launches == std::array{1, 1});
    observed->on_launch = {};
    observed->steps = {{0, completion(solution, 0)}, {0, completion(reference, 0)}};
    auto next = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    REQUIRE(next.outcome == CandidateOutcome::lost);
    REQUIRE(original.execution.programs[0].stdout_bytes == "retained");
    REQUIRE(recovered.execution.programs[0].stdout_bytes == "retained");
}
TEST_CASE("each recovery attempt shares its bound and leaves unresolved ownership inspectable", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](ProgramRole p) { if (p == reference) source.request_stop(); };
    script->steps = {{1000, {}}};
    Runner runner(std::move(script));
    auto original = runner.run("0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    observed->steps = {{99, {}}, {1, {}}};
    auto expired = runner.cleanup(std::chrono::milliseconds(100));
    const auto pending = runner.pending_ownership();
    auto refused = runner.run("0\n\n", {"/s", {}}, {"/r", {}}, {});
    observed->steps = {{0, completion(solution, 0)}, {0, completion(reference, 0)}};
    auto recovered = runner.cleanup();
    REQUIRE_FALSE(expired.resolved);
    REQUIRE(expired.errors[0].code == RunnerErrorCode::cleanup);
    REQUIRE(observed->waits[1] == 5);
    REQUIRE(observed->waits[2] == 1);
    REQUIRE(pending.size() == 2);
    REQUIRE(pending[0].awaiting_reap);
    REQUIRE(pending[0].open_descriptors == 3);
    REQUIRE(pending[0].process_id == 7000);
    REQUIRE(refused.errors[0].code == RunnerErrorCode::pending_ownership);
    REQUIRE(recovered.resolved);
    REQUIRE(original.errors[0].code == RunnerErrorCode::cleanup);
    REQUIRE(observed->launches == std::array{1, 1});
}
TEST_CASE("recovery retains cancellation observed with final resource completion", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource initial, retry;
    script->on_launch = [&](ProgramRole p) { if (p == reference) initial.request_stop(); };
    script->steps = {{1000, {}}};
    Runner runner(std::move(script));
    auto original = runner.run("0\n\n", {"/s", {}}, {"/r", {}}, {}, initial.token());
    auto events = completion(solution, 0); auto reference_events = completion(reference, 0);
    events.insert(events.end(), reference_events.begin(), reference_events.end());
    observed->steps = {{0, events}};
    observed->after_service = [&] { retry.request_stop(); };
    auto result = runner.cleanup(std::chrono::milliseconds(100), retry.token());
    REQUIRE(result.resolved);
    REQUIRE(result.cancelled);
    REQUIRE(result.errors.empty());
    REQUIRE(original.cancelled);
    REQUIRE(original.errors[0].code == RunnerErrorCode::cleanup);
}
TEST_CASE("recovery can resolve descriptor ownership after reaping without relaunching", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->on_launch = [&](ProgramRole p) { if (p == solution) observed->uncertain_close[0] = true; };
    auto events = completion(solution, 0); auto reference_events = completion(reference, 0);
    events.insert(events.end(), reference_events.begin(), reference_events.end());
    events.push_back({solution, EventKind::error, {}, EINTR, RunnerErrorCode::cleanup, "uncertain close"});
    script->steps = {{0, events}, {1000, {}}};
    Runner runner(std::move(script));
    auto original = runner.run("0\n\n", {"/s", {}}, {"/r", {}}, {});
    const auto pending = runner.pending_ownership();
    observed->steps = {{0, {}}};
    observed->after_service = [&] { observed->uncertain_close[0] = false; };
    auto recovered = runner.cleanup(std::chrono::milliseconds(0));
    REQUIRE(pending.size() == 1);
    REQUIRE_FALSE(pending[0].awaiting_reap);
    REQUIRE(pending[0].open_descriptors == 0);
    REQUIRE(pending[0].uncertain_descriptors == 1);
    REQUIRE(recovered.resolved);
    REQUIRE(recovered.errors.empty());
    REQUIRE(recovered.execution.programs[0].reaped);
    REQUIRE(original.errors[0].system_error == EINTR);
    REQUIRE(observed->launches == std::array{1, 1});
}
TEST_CASE("cancelled recovery intercepts waits and only stops still-owned programs", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource initial, retry;
    script->on_launch = [&](ProgramRole p) { if (p == reference) initial.request_stop(); };
    script->steps = {{0, completion(solution, 0)}, {1000, {}}};
    Runner runner(std::move(script));
    runner.run("0\n\n", {"/s", {}}, {"/r", {}}, {}, initial.token());
    retry.request_stop();
    observed->steps = {{0, {}}};
    auto cancelled = runner.cleanup(std::chrono::milliseconds(1000), retry.token());
    const auto stopped_after_cancel = observed->stops;
    const auto pending = runner.pending_ownership();
    observed->steps = {{0, completion(reference, 0)}};
    auto recovered = runner.cleanup();
    REQUIRE_FALSE(cancelled.resolved);
    REQUIRE(cancelled.cancelled);
    REQUIRE(cancelled.errors.empty());
    REQUIRE(observed->waits[2] == 0);
    REQUIRE(stopped_after_cancel == std::array{1, 2});
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].program == reference);
    REQUIRE(recovered.resolved);
    REQUIRE_FALSE(recovered.cancelled);
    REQUIRE(observed->launches == std::array{1, 1});
}
TEST_CASE("destroying a runner with pending ownership invokes the contract check without waiting", "[execution-events]") {
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::set_terminate([] { _exit(86); });
        auto script = std::make_unique<Script>();
        CancellationSource source;
        script->on_launch = [&](ProgramRole p) { if (p == reference) source.request_stop(); };
        script->steps = {{1000, {}}};
        {
            Runner runner(std::move(script));
            runner.run("0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
        }
        _exit(99);
    }
    int status = 0;
    pid_t reaped = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        reaped = waitpid(child, &status, WNOHANG);
        if (reaped != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (reaped == 0) {
        kill(child, SIGKILL);
        const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        do {
            reaped = waitpid(child, &status, WNOHANG);
            if (reaped != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < cleanup_deadline);
    }
    REQUIRE(reaped == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 86);
}
TEST_CASE("invalid cleanup bounds leave pending executions untouched", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](ProgramRole p) { if (p == reference) source.request_stop(); };
    script->steps = {{1000, {}}};
    Runner runner(std::move(script));
    runner.run("0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    auto negative = runner.cleanup(std::chrono::milliseconds(-1));
    auto excessive = runner.cleanup(std::chrono::milliseconds(1001));
    const auto stops = observed->stops;
    const auto waits = observed->waits.size();
    auto events = completion(solution, 0); auto reference_events = completion(reference, 0);
    events.insert(events.end(), reference_events.begin(), reference_events.end());
    observed->steps = {{0, events}};
    auto resolved = runner.cleanup(std::chrono::milliseconds(0));
    REQUIRE(negative.errors[0].code == RunnerErrorCode::configuration);
    REQUIRE(excessive.errors[0].code == RunnerErrorCode::configuration);
    REQUIRE_FALSE(negative.resolved);
    REQUIRE_FALSE(excessive.resolved);
    REQUIRE(stops == std::array{1, 1});
    REQUIRE(waits == 1);
    REQUIRE(resolved.resolved);
    REQUIRE(observed->waits.back() == 0);
}
TEST_CASE("failed recovery termination can be retried without erasing either error result", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](ProgramRole p) { if (p == reference) source.request_stop(); };
    script->fail_stop = solution;
    script->steps = {{1000, {}}};
    Runner runner(std::move(script));
    auto original = runner.run("0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    observed->steps = {{0, completion(reference, 0)}, {1000, {}}};
    auto failed = runner.cleanup();
    const auto pending = runner.pending_ownership();
    observed->fail_stop.reset();
    observed->steps = {{0, completion(solution, 0)}};
    auto recovered = runner.cleanup();
    REQUIRE_FALSE(failed.resolved);
    REQUIRE(failed.errors[0].system_error == EPERM);
    REQUIRE(failed.errors[1].code == RunnerErrorCode::cleanup);
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].program == solution);
    REQUIRE(recovered.resolved);
    REQUIRE(recovered.errors.empty());
    REQUIRE(recovered.execution.errors[0].system_error == EPERM);
    REQUIRE(original.errors[0].system_error == EPERM);
    REQUIRE_FALSE(original.programs[0].cleanup_complete);
    REQUIRE_FALSE(failed.execution.programs[0].cleanup_complete);
    REQUIRE(observed->launches == std::array{1, 1});
}
