if(NOT DEFINED RUNTIME OR NOT EXISTS "${RUNTIME}")
    message(FATAL_ERROR "RUNTIME must name the built Concept runtime")
endif()

set(forbidden_strings
    "concept runtime: "
    "Concept VM operand stack underflow"
    "truncated Concept bytecode"
    "division by zero in Concept program"
    "Concept VM pointer is null or invalid"
    "executable has no Concept bytecode payload"
    "Concept VM encountered an invalid opcode"
    "crypt32.dll"
    "CertVerifyCertificateChainPolicy"
)

foreach(plaintext IN LISTS forbidden_strings)
    file(STRINGS "${RUNTIME}" matches REGEX "${plaintext}" LIMIT_COUNT 1)
    if(matches)
        message(FATAL_ERROR
            "Native runtime contains plaintext string: ${plaintext}")
    endif()
endforeach()

execute_process(
    COMMAND "${RUNTIME}"
    RESULT_VARIABLE runtime_result
    OUTPUT_VARIABLE runtime_output
    ERROR_VARIABLE runtime_error
)

if(NOT runtime_result EQUAL 1)
    message(FATAL_ERROR
        "Unpackaged runtime returned ${runtime_result}, expected 1")
endif()

if(NOT runtime_error MATCHES
       "concept runtime: executable has no Concept bytecode payload")
    message(FATAL_ERROR
        "Runtime did not decrypt its diagnostic correctly:\n${runtime_error}")
endif()
