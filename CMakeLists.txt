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
    src/core/reduction.cpp
    src/core/coordinator.cpp
    src/core/evaluation_cache.cpp
    src/core/evaluator.cpp
    src/core/runner.cpp
)
target_include_directories(shrink_core PUBLIC src/core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(shrink_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(shrink src/cli/main.cpp src/cli/cli.cpp src/cli/options.cpp src/cli/saving.cpp src/cli/presentation.cpp src/cli/terminal.cpp)
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
    add_executable(shrink_core_tests tests/reduction.test.cpp tests/version.test.cpp tests/input.test.cpp tests/runner.test.cpp tests/execution_events.test.cpp)
    target_compile_definitions(shrink_core_tests PRIVATE SHRINK_PROGRAM_FIXTURE="$<TARGET_FILE:shrink_program_fixture>")
    add_dependencies(shrink_core_tests shrink_program_fixture)
    target_link_libraries(shrink_core_tests PRIVATE shrink_core Catch2::Catch2WithMain)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
    catch_discover_tests(shrink_core_tests PROPERTIES TIMEOUT 15)

    add_executable(shrink_cli_tests tests/cli.test.cpp tests/cancellation.test.cpp tests/saving.test.cpp src/cli/cli.cpp src/cli/options.cpp src/cli/saving.cpp src/cli/presentation.cpp src/cli/terminal.cpp)
    target_include_directories(shrink_cli_tests PRIVATE src/cli)
    target_link_libraries(shrink_cli_tests PRIVATE shrink_core Catch2::Catch2WithMain)
    target_compile_definitions(shrink_cli_tests PRIVATE SHRINK_PROGRAM_FIXTURE="$<TARGET_FILE:shrink_program_fixture>" SHRINK_CLI_PATH="$<TARGET_FILE:shrink>")
    add_dependencies(shrink_cli_tests shrink shrink_program_fixture)
    catch_discover_tests(shrink_cli_tests PROPERTIES TIMEOUT 15)

    add_test(NAME cli_information
        COMMAND "${CMAKE_COMMAND}"
            "-DSHRINK_EXECUTABLE=$<TARGET_FILE:shrink>"
            "-DCHECK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-check"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_information.cmake")

    foreach(demo IN ITEMS duplicates width)
        foreach(role IN ITEMS solution reference)
            add_executable(demo_${demo}_${role} examples/${demo}/${role}.cpp)
            target_compile_options(demo_${demo}_${role} PRIVATE -Wall -Wextra -Wpedantic)
        endforeach()
        add_test(NAME demo_${demo}
            COMMAND "${CMAKE_COMMAND}"
                "-DDEMO=${demo}"
                "-DSHRINK_EXECUTABLE=$<TARGET_FILE:shrink>"
                "-DDEMO_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/examples/${demo}"
                "-DSOLUTION=$<TARGET_FILE:demo_${demo}_solution>"
                "-DREFERENCE=$<TARGET_FILE:demo_${demo}_reference>"
                "-DCHECK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/demo-${demo}-check"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_demonstration.cmake")
        set_tests_properties(demo_${demo} PROPERTIES TIMEOUT 30)
    endforeach()
endif()
