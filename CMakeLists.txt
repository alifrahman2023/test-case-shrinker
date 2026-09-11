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
set(SHRINK_SANITIZE "none" CACHE STRING "Sanitizer to build with: none | address | thread")
set_property(CACHE SHRINK_SANITIZE PROPERTY STRINGS none address thread)

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
# Sanitizers as a linkable target rather than global CMAKE_CXX_FLAGS, so individual
# targets can opt out. tests/helpers/child.cpp must: under ASan a deliberate SIGSEGV is
# intercepted and reported as a plain non-zero exit, which would defeat its own test.
# ---------------------------------------------------------------------------
add_library(shrink_sanitizers INTERFACE)
if(SHRINK_SANITIZE STREQUAL "address")
  target_compile_options(shrink_sanitizers INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(shrink_sanitizers INTERFACE -fsanitize=address,undefined)
elseif(SHRINK_SANITIZE STREQUAL "thread")
  target_compile_options(shrink_sanitizers INTERFACE
    -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(shrink_sanitizers INTERFACE -fsanitize=thread)
elseif(NOT SHRINK_SANITIZE STREQUAL "none")
  message(FATAL_ERROR "SHRINK_SANITIZE must be none, address or thread (got ${SHRINK_SANITIZE})")
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
  src/ir.cpp
  src/runner.cpp
  src/spec.cpp
  src/version.cpp)
target_include_directories(shrink_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(shrink_core
  PRIVATE shrink_warnings
  PUBLIC shrink_sanitizers)
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

  # Stand-in for a solution binary. No sanitizers, no shrink_core; see above.
  add_executable(shrink_test_child tests/helpers/child.cpp)
  target_link_libraries(shrink_test_child PRIVATE shrink_warnings)

  add_executable(shrink_tests
    tests/unit/ir_test.cpp
    tests/unit/runner_test.cpp
    tests/unit/spec_test.cpp
    tests/unit/version_test.cpp)
  target_link_libraries(shrink_tests PRIVATE
    shrink_core shrink_warnings Catch2::Catch2WithMain)
  target_compile_definitions(shrink_tests PRIVATE
    SHRINK_TEST_CHILD="$<TARGET_FILE:shrink_test_child>")
  add_dependencies(shrink_tests shrink_test_child)

  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(shrink_tests)
endif()
