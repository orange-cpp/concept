cmake_minimum_required(VERSION 3.24)

project(concept VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(concept_core
    src/bytecode.cpp
    src/package.cpp
    src/vm.cpp
)
target_include_directories(concept_core PUBLIC include)

add_library(concept_frontend
    src/compiler.cpp
)
target_include_directories(concept_frontend PUBLIC include)
target_link_libraries(concept_frontend PUBLIC concept_core)

add_executable(concept-runtime runtime/main.cpp)
target_link_libraries(concept-runtime PRIVATE concept_core)

add_executable(concept src/main.cpp)
target_link_libraries(concept PRIVATE concept_frontend)
add_dependencies(concept concept-runtime)

if(MINGW)
    foreach(target concept-runtime concept)
        target_link_options(${target} PRIVATE -static -static-libgcc
                                             -static-libstdc++)
    endforeach()
endif()

foreach(target concept_core concept_frontend concept-runtime concept)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(concept-tests tests/tests.cpp)
    target_link_libraries(concept-tests PRIVATE concept_frontend)
    if(MSVC)
        target_compile_options(concept-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(concept-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(MINGW)
        target_link_options(concept-tests PRIVATE -static -static-libgcc
                                                  -static-libstdc++)
    endif()
    add_test(NAME concept-tests COMMAND concept-tests)
    add_test(
        NAME concept-packaged-program
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:concept>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/answer.concept
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-smoke.exe
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_packaged.cmake
    )
    add_test(
        NAME concept-packaged-types
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:concept>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/types.concept
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-types-smoke.exe
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_packaged.cmake
    )
    add_test(
        NAME concept-packaged-calculator
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:concept>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/calculator.concept
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-calculator-smoke.exe
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/calculator-input.txt
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_calculator.cmake
    )
endif()
