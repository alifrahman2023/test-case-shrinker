#include <shrink/ir.hpp>
#include <shrink/spec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using shrink::collection_paths;
using shrink::erase_elems;
using shrink::keep_elems;
using shrink::Node;
using shrink::parse_input;
using shrink::parse_spec;
using shrink::Path;
using shrink::serialize;
using shrink::set_int;
using shrink::Spec;

namespace {

Spec spec_for(const std::string& dsl) {
  auto parsed = parse_spec(dsl);
  INFO("spec error: " << parsed.error);
  REQUIRE(parsed.ok());
  return parsed.value;
}

Node tree_for(const Spec& spec, const std::string& text) {
  auto parsed = parse_input(spec, text);
  INFO("input error: " << parsed.error);
  REQUIRE(parsed.ok());
  return parsed.value;
}

std::string rejection(const Spec& spec, const std::string& text) {
  auto parsed = parse_input(spec, text);
  INFO("unexpectedly parsed: " << text);
  REQUIRE_FALSE(parsed.ok());
  return parsed.error;
}

struct Sample {
  const char* preset;
  const char* text;
};

// One canonical input per preset. Canonical means serialize() reproduces it byte for byte.
const std::vector<Sample>& samples() {
  static const std::vector<Sample> kSamples = {
      {"array", "3\n1 2 3\n"},
      {"multi", "2\n3\n1 2 3\n1\n5\n"},
      {"graph", "4\n3\n1 2\n2 3\n3 4\n"},
      {"grid", "2\n3\nabc\ndef\n"},
      {"string", "3\nabc\n"},
      {"scalars", "1\n2\n3\n"},
  };
  return kSamples;
}

}  // namespace

TEST_CASE("every preset round-trips byte for byte", "[ir][roundtrip]") {
  for (const auto& sample : samples()) {
    INFO("preset " << sample.preset);
    const auto spec = spec_for(std::string{shrink::preset_spec(sample.preset)});
    const auto tree = tree_for(spec, sample.text);
    REQUIRE(serialize(spec, tree) == sample.text);
  }
}

TEST_CASE("serializing is idempotent regardless of input whitespace", "[ir][roundtrip]") {
  const auto spec = spec_for("n; a[n]");
  const auto messy = tree_for(spec, "  3\n\n   1\n2    3   \n\n");
  const std::string once = serialize(spec, messy);

  REQUIRE(once == "3\n1 2 3\n");
  const auto reparsed = tree_for(spec, once);
  REQUIRE(serialize(spec, reparsed) == once);
  REQUIRE(reparsed == messy);
}

TEST_CASE("a derived count occupies no node in the tree", "[ir][derived]") {
  const auto spec = spec_for("n; a[n]");
  const auto tree = tree_for(spec, "3\n1 2 3\n");

  REQUIRE(tree.kids.size() == 1);  // just the array; n is nowhere
  REQUIRE(tree.kids[0].tag == Node::Tag::Array);
  REQUIRE(tree.kids[0].kids.size() == 3);
}

// The claim the whole IR design rests on: an edit cannot leave a length header stale,
// because the length is not stored anywhere to go stale.
TEST_CASE("deleting elements updates the length header automatically", "[ir][derived]") {
  const auto spec = spec_for("n; a[n]");
  const auto tree = tree_for(spec, "5\n10 20 30 40 50\n");

  const auto paths = collection_paths(tree);
  REQUIRE(paths.size() == 1);

  REQUIRE(serialize(spec, erase_elems(tree, paths[0], {1, 3})) == "3\n10 30 50\n");
  REQUIRE(serialize(spec, erase_elems(tree, paths[0], {0, 1, 2, 3, 4})) == "0\n\n");
  REQUIRE(serialize(spec, keep_elems(tree, paths[0], {4})) == "1\n50\n");
  // Out-of-range and duplicate indices are ignored rather than corrupting the tree.
  REQUIRE(serialize(spec, erase_elems(tree, paths[0], {1, 1, 99, -1})) == "4\n10 30 40 50\n");
}

TEST_CASE("an emptied array still round-trips", "[ir][derived]") {
  const auto spec = spec_for("n; a[n]");
  const auto empty = tree_for(spec, "0\n\n");
  REQUIRE(empty.kids[0].kids.empty());
  REQUIRE(serialize(spec, empty) == "0\n\n");
}

TEST_CASE("dropping a repeat iteration updates its counter", "[ir][derived]") {
  const auto spec = spec_for("t { n; a[n] }");
  const auto tree = tree_for(spec, "3\n2\n1 2\n1\n7\n3\n4 5 6\n");

  REQUIRE(tree.kids.size() == 1);
  REQUIRE(tree.kids[0].tag == Node::Tag::Repeat);
  REQUIRE(tree.kids[0].kids.size() == 3);

  const Path repeat{0};
  REQUIRE(serialize(spec, erase_elems(tree, repeat, {0, 2})) == "1\n1\n7\n");

  // Each iteration carries its own n, so shrinking inside one leaves the others alone.
  const Path inner_array{0, 2, 0};
  REQUIRE(serialize(spec, erase_elems(tree, inner_array, {1})) == "3\n2\n1 2\n1\n7\n2\n4 6\n");
}

TEST_CASE("deleting a grid row updates the row count", "[ir][grid]") {
  const auto spec = spec_for("r; c; g[r][c]:char");
  const auto tree = tree_for(spec, "3\n3\nabc\ndef\nghi\n");

  REQUIRE(serialize(spec, erase_elems(tree, Path{0}, {1})) == "2\n3\nabc\nghi\n");
  // Width follows the surviving rows, so an emptied grid reports 0 columns.
  REQUIRE(serialize(spec, erase_elems(tree, Path{0}, {0, 1, 2})) == "0\n0\n");
}

TEST_CASE("tuple elements become records", "[ir][graph]") {
  const auto spec = spec_for("n; m; {u,v}[m] in 1..n");
  const auto tree = tree_for(spec, "4\n3\n1 2\n2 3\n3 4\n");

  REQUIRE(tree.kids.size() == 2);  // n is stored (it is a domain, not a count); m is not
  REQUIRE(tree.kids[0].tag == Node::Tag::Int);
  REQUIRE(tree.kids[0].ival == 4);
  REQUIRE(tree.kids[1].kids[0].tag == Node::Tag::Record);
  REQUIRE(tree.kids[1].kids[0].kids.size() == 2);

  REQUIRE(serialize(spec, erase_elems(tree, Path{1}, {1})) == "4\n2\n1 2\n3 4\n");
  REQUIRE(serialize(spec, set_int(tree, Path{0}, 9)) == "9\n3\n1 2\n2 3\n3 4\n");
}

TEST_CASE("set_int reaches values inside records", "[ir][edit]") {
  const auto spec = spec_for("n; m; {u,v}[m] in 1..n");
  const auto tree = tree_for(spec, "4\n2\n1 2\n3 4\n");

  REQUIRE(serialize(spec, set_int(tree, Path{1, 1, 0}, 2)) == "4\n2\n1 2\n2 4\n");
  REQUIRE(shrink::at(tree, Path{1, 1, 0})->ival == 3);
}

TEST_CASE("collection paths are found in a deterministic order", "[ir][edit]") {
  const auto spec = spec_for("t { n; a[n] }");
  const auto tree = tree_for(spec, "2\n2\n1 2\n1\n3\n");

  const auto paths = collection_paths(tree);
  REQUIRE(paths == std::vector<Path>{{0}, {0, 0, 0}, {0, 1, 0}});
}

// Plan section 15: a spec that does not match the file must be a hard error. Shrinking a
// file the spec misreads produces confident nonsense.
TEST_CASE("leftover input is a hard error", "[ir][reject]") {
  const auto spec = spec_for("n; a[n]");
  REQUIRE(rejection(spec, "3\n1 2 3\n99\n").find("left over") != std::string::npos);
}

TEST_CASE("truncated input is a hard error", "[ir][reject]") {
  const auto spec = spec_for("n; a[n]");
  REQUIRE(rejection(spec, "3\n1 2\n").find("input ended") != std::string::npos);
  REQUIRE_FALSE(rejection(spec, "").empty());
}

TEST_CASE("a value outside its declared range is a hard error", "[ir][reject]") {
  const auto spec = spec_for("n; m; {u,v}[m] in 1..n");
  const auto error = rejection(spec, "3\n1\n1 9\n");
  REQUIRE(error.find("outside the declared range 1..3") != std::string::npos);
}

TEST_CASE("non-numeric and ragged input is rejected", "[ir][reject]") {
  const auto ints = spec_for("n; a[n]");
  REQUIRE(rejection(ints, "2\nx 1\n").find("expects an integer") != std::string::npos);

  const auto grid = spec_for("r; c; g[r][c]:char");
  REQUIRE(rejection(grid, "2\n3\nabc\nde\n").find("should be 3 characters") != std::string::npos);

  const auto text = spec_for("n; s[n]:char");
  REQUIRE(rejection(text, "3\nabcd\n").find("should be 3 characters") != std::string::npos);
}

TEST_CASE("the tree dump names fields", "[ir][dump]") {
  const auto spec = spec_for("n; a[n]");
  const auto text = dump(spec, tree_for(spec, "2\n7 8\n"));

  REQUIRE(text.find("array a (2)") != std::string::npos);
  REQUIRE(text.find("int a = 7") != std::string::npos);
}
