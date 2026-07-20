execute_process(
    COMMAND "${COMPILER}" "${SOURCE}" -o "${OUTPUT}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_output
    ERROR_VARIABLE compile_error
)

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "Concept compiler failed (${compile_result})\n${compile_output}${compile_error}")
endif()

execute_process(
    COMMAND "${OUTPUT}"
    RESULT_VARIABLE program_result
    OUTPUT_VARIABLE program_output
    ERROR_VARIABLE program_error
)

if(NOT program_result EQUAL 42)
    message(FATAL_ERROR
        "Packaged program returned ${program_result}, expected 42\n"
        "${program_output}${program_error}")
endif()
