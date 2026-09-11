cmake_minimum_required(VERSION 3.24)

project(shrink
  VERSION 0.1.0
  DESCRIPTION "Format-aware test-case reducer for competitive programming"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SHRINK_BUILD_TESTS "Build the test suite" ON)
option(SHRINK_WERROR "Treat warnings as errors on shrink's own targets" ON)

# ---------------------------------------------------------------------------
# Warning flags. Applied to our own targets only, never to fetched dependencies.
# ---------------------------------------------------------------------------
add_library(shrink_warnings INTERFACE)
target_compile_options(shrink_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(SHRINK_WERROR)
  target_compile_options(shrink_warnings INTERFACE -Werror)
endif()

# ---------------------------------------------------------------------------
# Dependencies (pinned by tag; never a branch)
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG        v2.4.2
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(cli11)

# ---------------------------------------------------------------------------
# Core library. Tests link this directly; main.cpp stays a thin shell.
# ---------------------------------------------------------------------------
add_library(shrink_core STATIC
  src/version.cpp)
target_include_directories(shrink_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(shrink_core PRIVATE shrink_warnings)
target_compile_definitions(shrink_core PRIVATE
  SHRINK_VERSION="${PROJECT_VERSION}")

add_executable(shrink src/main.cpp)
target_link_libraries(shrink PRIVATE shrink_core shrink_warnings CLI11::CLI11)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SHRINK_BUILD_TESTS)
  enable_testing()

  FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(catch2)

  add_executable(shrink_tests
    tests/unit/version_test.cpp)
  target_link_libraries(shrink_tests PRIVATE
    shrink_core shrink_warnings Catch2::Catch2WithMain)

  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(shrink_tests)
endif()
