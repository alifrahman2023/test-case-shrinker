# Verify a CLI-only source installation, including both real demonstrations.
# SOURCE_DIRECTORY may be an extracted tagged archive; no Git metadata is needed.
foreach(required SOURCE_DIRECTORY CHECK_DIRECTORY DEMO_BUILD_DIRECTORY)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

function(checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE status)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "Command failed (${status}): ${ARGV}")
    endif()
endfunction()

set(release "${CHECK_DIRECTORY}/release")
set(prefix "${CHECK_DIRECTORY}/install")
checked("${CMAKE_COMMAND}" -S "${SOURCE_DIRECTORY}" -B "${release}"
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF)
checked("${CMAKE_COMMAND}" --build "${release}" --parallel 3)
checked("${CMAKE_COMMAND}" --install "${release}" --prefix "${prefix}")
file(GLOB_RECURSE installed LIST_DIRECTORIES false RELATIVE "${prefix}" "${prefix}/*")
if(NOT installed STREQUAL "bin/shrink")
    message(FATAL_ERROR "Expected only bin/shrink, installed: ${installed}")
endif()
if(EXISTS "${release}/_deps")
    message(FATAL_ERROR "CLI-only installation fetched test dependencies")
endif()
checked("${CMAKE_COMMAND}" "-DSHRINK_EXECUTABLE=${prefix}/bin/shrink"
    "-DCHECK_DIRECTORY=${CHECK_DIRECTORY}/outside-build"
    -P "${SOURCE_DIRECTORY}/tests/check_information.cmake")
foreach(demo duplicates width)
    checked("${CMAKE_COMMAND}" "-DDEMO=${demo}"
        "-DSHRINK_EXECUTABLE=${prefix}/bin/shrink"
        "-DDEMO_DIRECTORY=${SOURCE_DIRECTORY}/examples/${demo}"
        "-DSOLUTION=${DEMO_BUILD_DIRECTORY}/demo_${demo}_solution"
        "-DREFERENCE=${DEMO_BUILD_DIRECTORY}/demo_${demo}_reference"
        "-DCHECK_DIRECTORY=${CHECK_DIRECTORY}/outside-build/${demo}"
        -P "${SOURCE_DIRECTORY}/tests/check_demonstration.cmake")
endforeach()
