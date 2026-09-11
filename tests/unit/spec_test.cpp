#include <shrink/spec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using shrink::Field;
using shrink::parse_spec;
using shrink::Spec;
using shrink::Stmt;

namespace {

Spec ok(const std::string& dsl) {
  auto parsed = parse_spec(dsl);
  INFO("spec: " << dsl << "\nerror: " << parsed.error);
  REQUIRE(parsed.ok());
  return parsed.value;
}

std::string rejection(const std::string& dsl) {
  auto parsed = parse_spec(dsl);
  INFO("spec: " << dsl);
  REQUIRE_FALSE(parsed.ok());
  return parsed.error;
}

const Field& field_of(const Spec& spec, const std::string& name) {
  const int idx = spec.field_index(name);
  REQUIRE(idx >= 0);
  return spec.field(idx);
}

}  // namespace

TEST_CASE("every preset parses", "[spec][preset]") {
  for (const auto& name : shrink::preset_names()) {
    const auto dsl = shrink::preset_spec(name);
    INFO("preset " << name);
    REQUIRE_FALSE(dsl.empty());
    REQUIRE(parse_spec(dsl).ok());
  }
  REQUIRE(shrink::preset_spec("nonesuch").empty());
}

// The central trick: a name used as a size stops being a value and becomes a count.
TEST_CASE("a field used as a size becomes derived", "[spec][derived]") {
  const auto spec = ok("n; a[n]");

  REQUIRE(field_of(spec, "n").role == Field::Role::Derived);
  REQUIRE_FALSE(field_of(spec, "n").stored());
  REQUIRE(field_of(spec, "a").role == Field::Role::Free);

  // The derived scalar claims no slot among its scope's children, so nothing in the tree
  // can hold a stale copy of it.
  REQUIRE(spec.stmts.size() == 2);
  REQUIRE(spec.stmts[0].kid_index == -1);
  REQUIRE(spec.stmts[1].kid_index == 0);
  REQUIRE(Spec::stored_count(spec.stmts) == 1);
}

TEST_CASE("a repeat declares its own counter", "[spec][derived]") {
  const auto spec = ok("t { n; a[n] }");

  REQUIRE(field_of(spec, "t").role == Field::Role::Derived);
  REQUIRE(field_of(spec, "n").role == Field::Role::Derived);
  REQUIRE(spec.stmts.size() == 1);
  REQUIRE(spec.stmts[0].kind == Stmt::Kind::Repeat);
  REQUIRE(spec.stmts[0].body.size() == 2);
  REQUIRE(spec.stmts[0].body[0].kid_index == -1);  // n
  REQUIRE(spec.stmts[0].body[1].kid_index == 0);   // a
}

TEST_CASE("a range bound marks a label domain", "[spec][domain]") {
  const auto spec = ok("n; m; {u,v}[m] in 1..n");

  REQUIRE(field_of(spec, "m").role == Field::Role::Derived);
  REQUIRE(field_of(spec, "n").role == Field::Role::Domain);

  const auto& u = field_of(spec, "u");
  REQUIRE(u.lo == 1);
  REQUIRE(u.hi_field == spec.field_index("n"));
  REQUIRE(field_of(spec, "v").hi_field == u.hi_field);

  REQUIRE(spec.stmts.size() == 3);
  REQUIRE(spec.stmts[2].kind == Stmt::Kind::Array);
  REQUIRE(spec.stmts[2].fields.size() == 2);
}

TEST_CASE("a grid derives both dimensions", "[spec][grid]") {
  const auto spec = ok("r; c; g[r][c]:char");

  REQUIRE(field_of(spec, "r").role == Field::Role::Derived);
  REQUIRE(field_of(spec, "c").role == Field::Role::Derived);
  REQUIRE(field_of(spec, "g").type == Field::Type::Char);
  REQUIRE(spec.stmts[2].kind == Stmt::Kind::Grid);
}

TEST_CASE("types and literal sizes parse", "[spec]") {
  const auto spec = ok("name:str; a[3]; b[2]:char; k in 0..100");

  REQUIRE(field_of(spec, "name").type == Field::Type::Str);
  REQUIRE(field_of(spec, "b").type == Field::Type::Char);
  REQUIRE(spec.stmts[1].size.is_fixed());
  REQUIRE(spec.stmts[1].size.fixed == 3);
  REQUIRE(field_of(spec, "k").lo == 0);
  REQUIRE(field_of(spec, "k").hi == 100);
}

TEST_CASE("a trailing semicolon is allowed", "[spec]") {
  REQUIRE(parse_spec("a; b;").ok());
}

// Every rejection below would otherwise become a confidently wrong reduction.
TEST_CASE("a size must be declared before it is used", "[spec][reject]") {
  REQUIRE(rejection("a[n]").find("before it is declared") != std::string::npos);
  REQUIRE(rejection("a[n]; n").find("before it is declared") != std::string::npos);
}

// Two arrays sharing one count cannot be represented: deleting from one would leave the
// other's length wrong, which is exactly the failure the derived-field design exists to
// prevent. Refusing the spec beats emitting malformed candidates.
TEST_CASE("two collections cannot share one length field", "[spec][reject]") {
  REQUIRE(rejection("n; a[n]; b[n]").find("cannot share one length field") != std::string::npos);
}

TEST_CASE("a size must belong to the block it counts", "[spec][reject]") {
  REQUIRE(rejection("k; t { a[k] }").find("same block") != std::string::npos);
}

TEST_CASE("malformed specs are rejected with a reason", "[spec][reject]") {
  REQUIRE_FALSE(rejection("a; a").empty());
  REQUIRE_FALSE(rejection("").empty());
  REQUIRE_FALSE(rejection("a[").empty());
  REQUIRE_FALSE(rejection("a[3]:bogus").empty());
  REQUIRE_FALSE(rejection("a in 1..").empty());
  REQUIRE_FALSE(rejection("t { }").empty());
  REQUIRE_FALSE(rejection("in").empty());
  REQUIRE_FALSE(rejection("a b").empty());
}

TEST_CASE("int grids are refused rather than silently mishandled", "[spec][reject]") {
  REQUIRE(rejection("r; c; g[r][c]:int").find(":char") != std::string::npos);
}
