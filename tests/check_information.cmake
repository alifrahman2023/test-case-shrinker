if(NOT DEFINED SHRINK_EXECUTABLE OR NOT DEFINED CHECK_DIRECTORY)
    message(FATAL_ERROR "SHRINK_EXECUTABLE and CHECK_DIRECTORY are required")
endif()
file(MAKE_DIRECTORY "${CHECK_DIRECTORY}")

execute_process(
    COMMAND "${SHRINK_EXECUTABLE}" --help
    WORKING_DIRECTORY "${CHECK_DIRECTORY}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors
    TIMEOUT 10
)
if(NOT "${status}" STREQUAL "0" OR NOT errors STREQUAL ""
   OR NOT output MATCHES "Usage: shrink" OR NOT output MATCHES "--help"
   OR NOT output MATCHES "--version")
    message(FATAL_ERROR "--help failed (${status}): ${output}${errors}")
endif()

execute_process(
    COMMAND "${SHRINK_EXECUTABLE}" --version
    WORKING_DIRECTORY "${CHECK_DIRECTORY}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors
    TIMEOUT 10
)
if(NOT "${status}" STREQUAL "0" OR NOT errors STREQUAL ""
   OR NOT output STREQUAL "shrink 0.1.0\n")
    message(FATAL_ERROR "--version failed (${status}): ${output}${errors}")
endif()
