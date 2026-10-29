if(NOT DEMO STREQUAL "duplicates" AND NOT DEMO STREQUAL "width")
    message(FATAL_ERROR "DEMO must be duplicates or width")
endif()
file(MAKE_DIRECTORY "${CHECK_DIRECTORY}")

function(check_answers input expected_solution expected_reference)
    foreach(role IN ITEMS SOLUTION REFERENCE)
        execute_process(COMMAND "${${role}}" INPUT_FILE "${input}"
            RESULT_VARIABLE status OUTPUT_VARIABLE answer ERROR_VARIABLE errors TIMEOUT 5)
        string(TOLOWER "${role}" lower_role)
        if(NOT "${status}" STREQUAL "0" OR NOT errors STREQUAL ""
           OR NOT answer STREQUAL "${expected_${lower_role}}\n")
            message(FATAL_ERROR "${role} on ${input}: status=${status}, answer=${answer}, stderr=${errors}")
        endif()
    endforeach()
endfunction()

function(check_input input)
    # Independently parse normalized text without shrink's format/constraint code.
    file(READ "${input}" text)
    string(REGEX MATCHALL "-?[0-9]+" numbers "${text}")
    list(POP_FRONT numbers length)
    list(LENGTH numbers actual_length)
    list(JOIN numbers " " value_line)
    if(NOT text STREQUAL "${length}\n${value_line}\n"
       OR NOT length MATCHES "^(0|[1-9][0-9]*)$")
        message(FATAL_ERROR "Input is not normalized: ${text}")
    endif()
    if(NOT length EQUAL actual_length OR length LESS min_length OR length GREATER max_length)
        message(FATAL_ERROR "Input length violates declared constraints: ${text}")
    endif()
    foreach(value IN LISTS numbers)
        if(NOT value MATCHES "^(0|-?[1-9][0-9]*)$"
           OR value LESS min_value OR value GREATER max_value)
            message(FATAL_ERROR "Input value violates declared constraints: ${text}")
        endif()
    endforeach()
    set(values "${numbers}" PARENT_SCOPE)
endfunction()

if(DEMO STREQUAL "width")
    check_answers("${DEMO_DIRECTORY}/original.txt" 1410065408 10000000000)
    set(min_length 2)
    set(max_length 2)
    set(min_value 0)
    set(max_value 100000)
else()
    check_answers("${DEMO_DIRECTORY}/original.txt" 4 3)
    set(min_length 0)
    set(max_length 10)
    set(min_value -100)
    set(max_value 100)
endif()
set(bounds --min-length ${min_length} --max-length ${max_length}
    --min-value=${min_value} --max-value ${max_value})
check_input("${DEMO_DIRECTORY}/original.txt")
execute_process(COMMAND "${SHRINK_EXECUTABLE}"
    --input "${DEMO_DIRECTORY}/original.txt" --solution "${SOLUTION}" --reference "${REFERENCE}"
    ${bounds} --output "${CHECK_DIRECTORY}/reduced.txt" --quiet
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE evidence TIMEOUT 20)
if(NOT "${status}" STREQUAL "0" OR NOT output STREQUAL "")
    message(FATAL_ERROR "Reduction failed (${status}): ${output}${evidence}")
endif()
file(WRITE "${CHECK_DIRECTORY}/evidence.txt" "${evidence}")
foreach(required IN ITEMS
    "Baseline: both programs succeeded; first differing token: 1"
    "Final: both programs succeeded; first differing token: 1"
    "Stopped: completed." "distinct inconclusive candidates: 0 (complete round)."
    "Irreducible under supported local checks:" "Final save: succeeded")
    string(FIND "${evidence}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Missing evidence '${required}': ${evidence}")
    endif()
endforeach()

check_input("${CHECK_DIRECTORY}/reduced.txt")
file(READ "${CHECK_DIRECTORY}/reduced.txt" saved)
if(DEMO STREQUAL "width")
    list(GET values 0 first)
    list(GET values 1 second)
    math(EXPR product "${first} * ${second}")
    # The mathematical product is the independent oracle, computed at 64 bits.
    # Local irreducibility requires both one-unit moves to lose the disagreement.
    math(EXPR first_step "(${first} - 1) * ${second}")
    math(EXPR second_step "${first} * (${second} - 1)")
    if(product LESS_EQUAL 4294967295 OR first_step GREATER 4294967295
       OR second_step GREATER 4294967295)
        message(FATAL_ERROR "Width result is not a locally irreducible wrong answer: ${saved}")
    endif()
    math(EXPR wrapped "${product} % 4294967296")
    check_answers("${CHECK_DIRECTORY}/reduced.txt" "${wrapped}" "${product}")
else()
    # This fixture's visitation order determines [0, 1, 0]. Removing any element,
    # changing both zeros to one, or changing the one to zero loses the disagreement.
    if(NOT saved STREQUAL "3\n0 1 0\n")
        message(FATAL_ERROR "Unexpected duplicate-handling result: ${saved}")
    endif()
    check_answers("${CHECK_DIRECTORY}/reduced.txt" 3 2)
endif()

# Recovery is a fresh verification, with all required options and a new destination.
execute_process(COMMAND "${SHRINK_EXECUTABLE}"
    --input "${CHECK_DIRECTORY}/reduced.txt" --solution "${SOLUTION}" --reference "${REFERENCE}"
    ${bounds} --output "${CHECK_DIRECTORY}/recovered.txt" --max-evaluations 0 --quiet
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE evidence TIMEOUT 10)
file(WRITE "${CHECK_DIRECTORY}/recovery-evidence.txt" "${evidence}")
if(NOT "${status}" STREQUAL "4" OR NOT output STREQUAL ""
   OR NOT evidence MATCHES "Baseline evaluations: 1; candidate evaluations: 0;"
   OR NOT evidence MATCHES "No reduction round began"
   OR NOT evidence MATCHES "Final save: succeeded")
    message(FATAL_ERROR "Fresh recovery failed (${status}): ${output}${evidence}")
endif()
file(READ "${CHECK_DIRECTORY}/recovered.txt" recovered)
file(READ "${CHECK_DIRECTORY}/reduced.txt" unchanged)
if(NOT recovered STREQUAL saved OR NOT unchanged STREQUAL saved)
    message(FATAL_ERROR "Fresh verification changed the saved input")
endif()
