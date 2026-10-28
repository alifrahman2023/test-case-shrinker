#include "coordinator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <utility>

namespace {
shrink::ReductionRequest request(std::string text) {
    return {std::move(text), {0, INT64_MAX, INT64_MIN, INT64_MAX},
            {"./solution", {}}, {"./reference", {}}, {}};
}
}

TEST_CASE("A typed request normalizes array layout while preserving values and order", "[input]") {
    auto result = shrink::prepare_request(request(" \t3\n7 -2\t7 \n"));
    REQUIRE(std::holds_alternative<shrink::PreparedRequest>(result));
    const auto& prepared = std::get<shrink::PreparedRequest>(result);
    REQUIRE(prepared.values == std::vector<std::int64_t>{7, -2, 7});
    REQUIRE(prepared.normalized_input == "3\n7 -2 7\n");
    REQUIRE(prepared.request.input_text == " \t3\n7 -2\t7 \n");
}

TEST_CASE("Typed requests reject malformed integer arrays", "[input]") {
    for (const auto* text : {"", " ", "-1", "+1 2", "01 2", "-0", "9223372036854775808",
                            "1", "2 7", "0 extra", "1 7 8", "1 +1", "1 01", "1 -0", "1 -01",
                            "1 1.0", "1 1e2", "1 0x10", "1 1,000", "1 --1",
                            "1 9223372036854775808", "1 -9223372036854775809",
                            "9223372036854775807"}) {
        INFO(text);
        REQUIRE(std::holds_alternative<shrink::ValidationError>(shrink::prepare_request(request(text))));
    }
}

TEST_CASE("Typed requests accept empty arrays and the signed integer extremes", "[input]") {
    for (auto [text, normalized] : {std::pair{"0 \n\t", "0\n\n"},
                                 std::pair{"2 -9223372036854775808 9223372036854775807",
                                           "2\n-9223372036854775808 9223372036854775807\n"}}) {
        auto result = shrink::prepare_request(request(text));
        REQUIRE(std::holds_alternative<shrink::PreparedRequest>(result));
        REQUIRE(std::get<shrink::PreparedRequest>(result).normalized_input == normalized);
    }
}

TEST_CASE("Declared bounds ordering and uniqueness govern typed input validation", "[input]") {
    auto original = request("3 1 2 3");
    original.constraints = {1, 3, 1, 3, shrink::Ordering::ascending, true};
    REQUIRE(std::holds_alternative<shrink::PreparedRequest>(shrink::prepare_request(original)));
    for (auto text : {"0", "4 1 2 3 4", "3 0 2 3", "3 1 2 4", "3 2 1 3", "3 1 2 2"}) {
        auto invalid = original;
        invalid.input_text = text;
        INFO(text);
        REQUIRE(std::holds_alternative<shrink::ValidationError>(shrink::prepare_request(invalid)));
    }
    original.input_text = "3 3 2 1";
    original.constraints.ordering = shrink::Ordering::descending;
    REQUIRE(std::holds_alternative<shrink::PreparedRequest>(shrink::prepare_request(original)));
    original.input_text = "3 3 2 3";
    REQUIRE(std::holds_alternative<shrink::ValidationError>(shrink::prepare_request(original)));
    original.constraints.ordering = shrink::Ordering::unrestricted;
    REQUIRE(std::holds_alternative<shrink::ValidationError>(shrink::prepare_request(original)));
    original.constraints.unique = false;
    REQUIRE(std::holds_alternative<shrink::PreparedRequest>(shrink::prepare_request(original)));
    original.input_text = "3 3 3 2";
    original.constraints.ordering = shrink::Ordering::descending;
    REQUIRE(std::holds_alternative<shrink::PreparedRequest>(shrink::prepare_request(original)));
    original.constraints.ordering = shrink::Ordering::ascending;
    REQUIRE(std::holds_alternative<shrink::ValidationError>(shrink::prepare_request(original)));
}

TEST_CASE("Typed configuration rejects impossible bounds programs and limits independently of CLI", "[input]") {
    for (auto constraints : {shrink::InputConstraints{-1, 3, 0, 3}, {4, 3, 0, 3},
                             {0, 3, 3, 0}, {4, 4, 1, 3, shrink::Ordering::unrestricted, true},
                             {2, 2, INT64_MAX, INT64_MAX, shrink::Ordering::unrestricted, true},
                             {2, 2, INT64_MIN, INT64_MIN, shrink::Ordering::unrestricted, true}}) {
        auto invalid = request("0");
        invalid.constraints = constraints;
        auto result = shrink::prepare_request(invalid);
        REQUIRE(std::holds_alternative<shrink::ValidationError>(result));
        REQUIRE(std::get<shrink::ValidationError>(result).code == shrink::ValidationCode::configuration);
    }
    for (int field = 0; field < 7; ++field) {
        auto invalid = request("0");
        switch (field) {
        case 0: invalid.solution.executable.clear(); break;
        case 1: invalid.reference.executable.clear(); break;
        case 2: invalid.solution.arguments = {std::string("a\0b", 3)}; break;
        case 3: invalid.reference.executable = std::string("a\0b", 3); break;
        case 4: invalid.limits.solution_timeout = std::chrono::milliseconds(0); break;
        case 5: invalid.limits.reference_timeout = std::chrono::milliseconds(-1); break;
        case 6: invalid.limits.time_limit = std::chrono::milliseconds(0); break;
        }
        REQUIRE(std::holds_alternative<shrink::ValidationError>(shrink::prepare_request(invalid)));
    }
}

TEST_CASE("Uniqueness interval arithmetic handles ranges crossing zero and the full domain", "[input]") {
    auto original = request("3 -1 0 1");
    original.constraints = {3, 3, -1, 1, shrink::Ordering::ascending, true};
    REQUIRE(std::holds_alternative<shrink::PreparedRequest>(shrink::prepare_request(original)));
    original.constraints.min_value = INT64_MIN;
    original.constraints.max_value = INT64_MAX;
    REQUIRE(std::holds_alternative<shrink::PreparedRequest>(shrink::prepare_request(original)));
    original.constraints = {INT64_MAX, INT64_MAX, INT64_MIN, INT64_MAX,
                            shrink::Ordering::unrestricted, true};
    auto result = shrink::prepare_request(original);
    REQUIRE(std::holds_alternative<shrink::ValidationError>(result));
    REQUIRE(std::get<shrink::ValidationError>(result).code != shrink::ValidationCode::configuration);
    for (auto text : {"0", "1 -1"}) {
        for (auto ordering : {shrink::Ordering::ascending, shrink::Ordering::descending}) {
            original = request(text);
            original.constraints.ordering = ordering;
            original.constraints.unique = true;
            REQUIRE(std::holds_alternative<shrink::PreparedRequest>(shrink::prepare_request(original)));
        }
    }
}
