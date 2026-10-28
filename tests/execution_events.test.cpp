#include "evaluator.hpp"
#include "runner_backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <functional>

namespace {
using namespace shrink;
using namespace shrink::detail;
struct Step { std::uint64_t elapsed; std::vector<ExecutionEvent> events; };
class Script final : public ExecutionBackend {
public:
    std::uint64_t clock = 100;
    std::deque<Step> steps;
    std::array<bool, 2> active{};
    std::array<unsigned, 2> completion_parts{};
    std::array<bool, 2> uncertain_close{};
    std::function<void()> on_clock;
    std::array<int, 2> stops{};
    std::array<std::optional<std::uint64_t>, 2> stopped_at{};
    std::array<int, 2> launches{};
    std::vector<int> waits;
    std::array<std::uint64_t, 2> launch_delay{};
    std::function<void(int)> on_launch;
    std::optional<int> fail_launch;
    std::uint64_t now_ms() override { if (on_clock) on_clock(); return clock; }
    std::optional<RunnerError> launch(int p, const ProgramSpec&, std::string_view) override {
        ++launches[p]; clock += launch_delay[p];
        if (fail_launch == p) return RunnerError{RunnerErrorCode::launch, p, ENOENT, "scripted launch"};
        active[p] = true;
        if (on_launch) on_launch(p);
        return {};
    }
    bool pending(int p) const override { return active[p] || uncertain_close[p]; }
    std::optional<RunnerError> stop(int p) override { ++stops[p]; stopped_at[p] = clock; return {}; }
    std::vector<ExecutionEvent> service(CancellationToken, int wait_ms) override {
        waits.push_back(wait_ms);
        if (steps.empty()) throw std::runtime_error("script exhausted");
        auto step = std::move(steps.front()); steps.pop_front(); clock += step.elapsed;
        for (const auto& e : step.events) {
            if (e.program < 0) continue;
            auto& parts = completion_parts[e.program];
            if (e.kind == EventKind::setup_complete || (e.kind == EventKind::error && e.error_code == RunnerErrorCode::launch)) parts |= 1;
            if (e.kind == EventKind::stdout_closed) parts |= 2;
            if (e.kind == EventKind::stderr_closed) parts |= 4;
            if (e.kind == EventKind::exited || e.kind == EventKind::signalled) parts |= 8;
            if (parts == 15) active[e.program] = false;
        }
        return step.events;
    }
};
std::vector<ExecutionEvent> completion(int program, int code) {
    return {{program, EventKind::setup_complete}, {program, EventKind::stdout_closed},
            {program, EventKind::stderr_closed}, {program, EventKind::exited, {}, code}};
}
}
TEST_CASE("solution failure stops its reference promptly in either completion order", "[execution-events]") {
    for (int first : {0, 1}) {
        auto script = std::make_unique<Script>(); auto* observed = script.get();
        script->steps = {{0, completion(first, 7)}, {0, completion(1 - first, 9)}};
        Runner runner(std::move(script));
        auto result = evaluate_candidate(runner, "0\n\n", {"/solution", {}}, {"/reference", {}}, {});
        REQUIRE(result.outcome == CandidateOutcome::lost);
        REQUIRE(observed->stops[0] == 0);
        REQUIRE(observed->stops[1] == (first == 0 ? 1 : 0));
    }
}
TEST_CASE("deadline covers collection and distinguishes pending setup from execution timeout", "[execution-events]") {
    for (bool setup_done : {false, true}) {
        auto script = std::make_unique<Script>();
        if (setup_done) script->steps.push_back({0, {{0, EventKind::setup_complete}, {1, EventKind::setup_complete}}});
        script->steps.push_back({10, completion(0, 0)});
        script->steps.push_back({0, completion(1, 0)});
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
    REQUIRE_FALSE(observed->active[0]);
    REQUIRE_FALSE(observed->active[1]);
}
TEST_CASE("cancellation after only one launch stops that child and does not launch its peer", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](int) { source.request_stop(); };
    script->steps = {{0, completion(0, 0)}};
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
    script->steps = {{0, completion(0, 0)}};
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
    script->on_launch = [&](int p) { if (p == 1) source.request_stop(); };
    auto failed = completion(0, 7);
    failed.push_back({1, EventKind::error, {}, EIO, RunnerErrorCode::io, "scripted read failure"});
    script->steps = {{0, failed}, {0, completion(1, 0)}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.cancelled);
    REQUIRE(result.execution.errors[0].code == RunnerErrorCode::io);
    REQUIRE(result.execution.programs[0].exit_code == 7);
    REQUIRE(observed->stops == std::array{1, 1});
}
TEST_CASE("unresolved cleanup has one shared bound and retained ownership blocks new work", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    CancellationSource source;
    script->on_launch = [&](int p) { if (p == 1) source.request_stop(); };
    script->steps = {{1000, {}}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    const bool pending = runner.has_pending_ownership();
    auto refused = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    const auto launches = observed->launches;
    const auto clock = observed->clock;
    // Deliver eventual OS completion through the controlled execution seam.
    // The production runner still owns its backend throughout the failed call.
    observed->steps = {{0, completion(0, 0)}, {0, completion(1, 0)}};
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
    script->on_launch = [&](int p) { if (p == 1) source.request_stop(); };
    auto both = completion(0, 0); auto second = completion(1, 0);
    both.insert(both.end(), second.begin(), second.end());
    script->steps = {{999, {}}, {1, both}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {}, source.token());
    REQUIRE(result.execution.errors.empty());
    REQUIRE(observed->waits == std::vector<int>{5, 1});
}
TEST_CASE("reference failure is inconclusive only when solution succeeds in either order", "[execution-events]") {
    for (int first : {0, 1}) {
        auto script = std::make_unique<Script>(); auto* observed = script.get();
        script->steps = {{0, completion(first, first == 0 ? 0 : 127)},
                         {0, completion(1 - first, first == 0 ? 127 : 0)}};
        Runner runner(std::move(script));
        auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
        REQUIRE(result.execution.errors.empty());
        REQUIRE(result.outcome == CandidateOutcome::inconclusive);
        REQUIRE(observed->stops == std::array{0, 0});
    }
}
TEST_CASE("late operational errors override an already observed ordinary solution failure", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->steps = {{0, completion(0, 7)},
        {0, {{1, EventKind::error, {}, EACCES, RunnerErrorCode::launch, "exec"}}},
        {0, completion(1, 127)}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    REQUIRE_FALSE(result.outcome);
    REQUIRE(result.execution.errors[0].code == RunnerErrorCode::launch);
    REQUIRE(observed->stops[1] == 1);
    REQUIRE(result.execution.programs[0].exit_code == 7);
}
TEST_CASE("sleep-inclusive clock jumps expire work before newly collected completion", "[execution-events]") {
    auto script = std::make_unique<Script>(); auto* observed = script.get();
    script->steps = {{0, {{0, EventKind::setup_complete}, {1, EventKind::setup_complete}}},
                     {60'000, {}}, {0, completion(0, 0)}, {0, completion(1, 0)}};
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
    script->steps = {{0, {{0, EventKind::setup_complete}, {1, EventKind::setup_complete}}},
                     {1, {}}, {5, completion(1, 0)}, {2000, {}}, {0, completion(0, 0)}};
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
    script->steps = {{0, {{0, EventKind::setup_complete}, {1, EventKind::setup_complete},
                         {0, EventKind::stdout_data, "1"}, {1, EventKind::stdout_data, "1.0"},
                         {0, EventKind::exited, {}, 0}, {1, EventKind::exited, {}, 0}}},
                     {0, {{0, EventKind::stdout_data, ".0"}, {0, EventKind::stdout_closed}, {0, EventKind::stderr_closed},
                          {1, EventKind::stdout_closed}, {1, EventKind::stderr_closed}}}};
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
    script->steps = {{0, {{0, EventKind::setup_complete}, {1, EventKind::setup_complete},
                         {0, EventKind::exited, {}, 0}}},
                     {10, {{0, EventKind::stdout_data, "a"}, {0, EventKind::stdout_closed}, {0, EventKind::stderr_closed}}},
                     {0, completion(1, 0)}};
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
    std::vector<ExecutionEvent> both{{0, EventKind::stdout_data, "1"}, {1, EventKind::stdout_data, "1.0"}};
    auto first = completion(0, 0); auto second = completion(1, 0);
    both.insert(both.end(), first.begin(), first.end());
    both.insert(both.end(), second.begin(), second.end());
    script->steps = {{0, both}};
    CancellationSource source;
    int collection_clock_reads = 0;
    script->on_clock = [&] {
        if (observed->launches == std::array{1, 1} && !observed->pending(0) && !observed->pending(1)
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
    script->on_launch = [&](int p) { if (p == 0) observed->uncertain_close[0] = true; };
    auto events = completion(0, 0); auto reference = completion(1, 0);
    events.insert(events.end(), reference.begin(), reference.end());
    events.push_back({0, EventKind::error, {}, EINTR, RunnerErrorCode::cleanup, "descriptor release uncertain"});
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
    script->steps = {{0, {{0, EventKind::setup_complete}, {1, EventKind::setup_complete},
                         {0, EventKind::exited, {}, 7}}},
                     {100, {{0, EventKind::stdout_closed}, {0, EventKind::stderr_closed}}},
                     {0, completion(1, 0)}};
    Runner runner(std::move(script));
    auto result = evaluate_candidate(runner, "0\n\n", {"/s", {}}, {"/r", {}}, {});
    REQUIRE(result.outcome == CandidateOutcome::lost);
    REQUIRE(observed->stopped_at[1] == 100);
    REQUIRE(observed->stops[0] == 0);
    REQUIRE(result.execution.programs[0].output_complete);
}
