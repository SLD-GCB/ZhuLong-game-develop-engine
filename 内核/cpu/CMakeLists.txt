add_library(zlong_cpu STATIC
    src/arm64_interface.cpp
    src/core.cpp
    src/cpu.cpp
)
add_library(zlong::cpu ALIAS zlong_cpu)

target_include_directories(zlong_cpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# dynarmic propagates its PUBLIC include directory (external/dynarmic/src), so
# <dynarmic/interface/...> resolves for our translation units too.
target_link_libraries(zlong_cpu PUBLIC dynarmic)

target_compile_features(zlong_cpu PUBLIC cxx_std_20)
target_compile_options(zlong_cpu PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

option(ZL_BUILD_CPU_TESTS "Build the CPU-layer verification tests" ON)

if (ZL_BUILD_CPU_TESTS)
    add_executable(zlong_cpu_tests
        tests/test_main.cpp
        tests/test_single_core.cpp
        tests/test_exclusive.cpp
        tests/test_events.cpp
        tests/test_smc.cpp
        tests/test_interrupt.cpp
    )
    target_include_directories(zlong_cpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(zlong_cpu_tests PRIVATE zlong::cpu)
    target_compile_options(zlong_cpu_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_cpu_tests COMMAND zlong_cpu_tests)
endif()
