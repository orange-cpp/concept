execute_process(
    COMMAND "${COMPILER}" "${SOURCE}" -o "${OUTPUT}"
            --shared-module --vms 8
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_output
    ERROR_VARIABLE compile_error
)

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "Concept shared-module compilation failed (${compile_result})\n"
        "${compile_output}${compile_error}")
endif()

execute_process(
    COMMAND "${LOADER}" "${OUTPUT}"
    RESULT_VARIABLE loader_result
    OUTPUT_VARIABLE loader_output
    ERROR_VARIABLE loader_error
)

if(NOT loader_result EQUAL 42)
    message(FATAL_ERROR
        "Concept DLL loader returned ${loader_result}, expected 42\n"
        "${loader_output}${loader_error}")
endif()
