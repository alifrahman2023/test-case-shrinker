cmake_minimum_required(VERSION 3.24)
project(shrink VERSION 0.1.0 LANGUAGES CXX)

message(STATUS "CMake version: ${CMAKE_VERSION}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")

if(NOT APPLE OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR "shrink currently supports native macOS builds with Apple Clang")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

configure_file(cmake/version.hpp.in generated/shrink/version.hpp @ONLY)

add_library(shrink_core STATIC
    src/core/version.cpp
    src/core/input_format.cpp
    src/core/coordinator.cpp
    src/core/evaluator.cpp
    src/core/runner.cpp
)
target_include_directories(shrink_core PUBLIC src/core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(shrink_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(shrink src/cli/main.cpp src/cli/cli.cpp src/cli/options.cpp)
target_link_libraries(shrink PRIVATE shrink_core)
target_compile_options(shrink PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS shrink RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    # Catch2 v3.8.1; dependency updates are reviewed explicitly.
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(shrink_program_fixture tests/program_fixture.cpp)
    add_executable(shrink_core_tests tests/version.test.cpp tests/input.test.cpp tests/runner.test.cpp tests/execution_events.test.cpp)
    target_compile_definitions(shrink_core_tests PRIVATE SHRINK_PROGRAM_FIXTURE="$<TARGET_FILE:shrink_program_fixture>")
    add_dependencies(shrink_core_tests shrink_program_fixture)
    target_link_libraries(shrink_core_tests PRIVATE shrink_core Catch2::Catch2WithMain)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
    catch_discover_tests(shrink_core_tests PROPERTIES TIMEOUT 15)

    add_executable(shrink_cli_tests tests/cli.test.cpp src/cli/cli.cpp src/cli/options.cpp)
    target_include_directories(shrink_cli_tests PRIVATE src/cli)
    target_link_libraries(shrink_cli_tests PRIVATE shrink_core Catch2::Catch2WithMain)
    target_compile_definitions(shrink_cli_tests PRIVATE SHRINK_CLI_PATH="$<TARGET_FILE:shrink>")
    add_dependencies(shrink_cli_tests shrink)
    catch_discover_tests(shrink_cli_tests PROPERTIES TIMEOUT 15)

    add_test(NAME cli_information
        COMMAND "${CMAKE_COMMAND}"
            "-DSHRINK_EXECUTABLE=$<TARGET_FILE:shrink>"
            "-DCHECK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-check"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_information.cmake")
endif()
