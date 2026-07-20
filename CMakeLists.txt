cmake_minimum_required(VERSION 3.24)

project(concept VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
    xorstr
    URL https://github.com/JustasMasiulis/xorstr/archive/066c64eea5104f4e3cfbc49e39031400e086425a.tar.gz
    URL_HASH SHA256=EF1A6F103266EBEED102656A5AA2FE90F4DDA1F5F7A930FE6D2B82AB07A0B3E0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(xorstr)

add_library(concept_xorstr INTERFACE)
target_include_directories(concept_xorstr INTERFACE
    "${xorstr_SOURCE_DIR}/include"
)

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
target_link_libraries(concept_core PUBLIC concept_xorstr)
target_compile_definitions(concept_core PRIVATE
    JM_XORSTR_DISABLE_AVX_INTRINSICS
)

add_library(concept_frontend
    src/compiler.cpp
)
target_include_directories(concept_frontend PUBLIC include)
target_link_libraries(concept_frontend PUBLIC concept_core)

add_executable(concept-runtime runtime/main.cpp)
target_link_libraries(concept-runtime PRIVATE concept_core)
target_compile_definitions(concept-runtime PRIVATE
    JM_XORSTR_DISABLE_AVX_INTRINSICS
)

if(WIN32)
    add_library(concept-runtime-shared SHARED runtime/dllmain.cpp)
    target_link_libraries(concept-runtime-shared PRIVATE concept_core)
    target_compile_definitions(concept-runtime-shared PRIVATE
        JM_XORSTR_DISABLE_AVX_INTRINSICS
    )
    set_target_properties(concept-runtime-shared PROPERTIES
        PREFIX ""
        OUTPUT_NAME "concept-runtime-shared"
    )
endif()

add_executable(concept src/main.cpp)
target_link_libraries(concept PRIVATE concept_frontend)
add_custom_target(concept-modules
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/concept"
        "$<TARGET_FILE_DIR:concept-runtime>/concept"
)
add_dependencies(concept concept-runtime concept-modules)
if(WIN32)
    add_dependencies(concept concept-runtime-shared)
endif()

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

if(WIN32)
    if(MSVC)
        target_compile_options(concept-runtime-shared PRIVATE /W4 /permissive-)
    else()
        target_compile_options(concept-runtime-shared PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
endif()

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
        NAME concept-runtime-xorstr
        COMMAND ${CMAKE_COMMAND}
            -DRUNTIME=$<TARGET_FILE:concept-runtime>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_runtime_strings.cmake
    )
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
    add_test(
        NAME concept-packaged-sockets
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:concept>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/socket-loopback.concept
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-sockets-smoke.exe
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_packaged.cmake
    )
    add_test(
        NAME concept-packaged-classes
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:concept>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/classes.concept
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-classes-smoke.exe
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_packaged.cmake
    )
    add_test(
        NAME concept-packaged-pointers
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:concept>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/pointers.concept
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-pointers-smoke.exe
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_packaged.cmake
    )
    add_test(
        NAME concept-packaged-arrays
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:concept>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/arrays.concept
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-arrays-smoke.exe
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_packaged.cmake
    )
    add_test(
        NAME concept-packaged-dynamic-array
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:concept>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/dynamic-array.concept
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-dynamic-array-smoke.exe
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_packaged.cmake
    )
    add_test(
        NAME concept-packaged-arguments
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=$<TARGET_FILE:concept>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/arguments.concept
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-arguments-smoke.exe
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_packaged.cmake
    )
    if(WIN32)
        add_executable(concept-dll-loader tests/dll_loader.cpp)
        if(MSVC)
            target_compile_options(concept-dll-loader PRIVATE /W4 /permissive-)
        else()
            target_compile_options(concept-dll-loader PRIVATE
                -Wall -Wextra -Wpedantic)
        endif()
        add_test(
            NAME concept-packaged-shared-module
            COMMAND ${CMAKE_COMMAND}
                -DCOMPILER=$<TARGET_FILE:concept>
                -DLOADER=$<TARGET_FILE:concept-dll-loader>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/shared-module-smoke.concept
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/concept-shared-smoke.dll
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_shared_module.cmake
        )
    endif()
endif()
