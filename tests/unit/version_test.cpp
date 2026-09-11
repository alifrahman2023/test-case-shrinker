#include <shrink/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cctype>

TEST_CASE("version is a non-empty dotted string", "[version]") {
  const auto v = shrink::version();
  REQUIRE_FALSE(v.empty());
  REQUIRE(v.find('.') != std::string_view::npos);
  REQUIRE(std::isdigit(static_cast<unsigned char>(v.front())));
}

TEST_CASE("version banner names the tool and carries the version", "[version]") {
  const auto banner = shrink::version_banner();
  REQUIRE(banner.starts_with("shrink "));
  REQUIRE(banner.ends_with(shrink::version()));
}
