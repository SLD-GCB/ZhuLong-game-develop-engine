add_library(zlong_ram STATIC
    src/address_space.cpp
    src/mmu.cpp
    src/physical.cpp
    src/ram.cpp
)
add_library(zlong::ram ALIAS zlong_ram)

target_include_directories(zlong_ram PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# PUBLIC: this library implements zlong::cpu::GuestMemory, so consumers need the
# cpu interface headers too.
target_link_libraries(zlong_ram PUBLIC zlong::cpu)

target_compile_features(zlong_ram PUBLIC cxx_std_20)
target_compile_options(zlong_ram PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

option(ZL_BUILD_RAM_TESTS "Build the RAM-layer tests" ON)

if (ZL_BUILD_RAM_TESTS)
    add_executable(zlong_ram_tests
        tests/test_main.cpp
        tests/test_descriptor_format.cpp
        tests/test_guest_memory.cpp
        tests/test_mmu_faults.cpp
        tests/test_mmu_permissions.cpp
        tests/test_mmu_walk.cpp
        tests/test_sysreg.cpp
        tests/test_tlb.cpp
    )
    target_include_directories(zlong_ram_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(zlong_ram_tests PRIVATE zlong::ram)
    target_compile_options(zlong_ram_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_ram_tests COMMAND zlong_ram_tests)
endif()
