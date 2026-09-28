add_library(zlong_ssd STATIC
    src/block_device.cpp
    src/container.cpp
)
add_library(zlong::ssd ALIAS zlong_ssd)

target_include_directories(zlong_ssd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(zlong_ssd PUBLIC cxx_std_20)
target_compile_options(zlong_ssd PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

option(ZL_BUILD_SSD_TESTS "Build the SSD-layer tests" ON)

if (ZL_BUILD_SSD_TESTS)
    add_executable(zlong_ssd_tests
        tests/test_main.cpp
        tests/test_block_device.cpp
        tests/test_container.cpp
    )
    target_include_directories(zlong_ssd_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(zlong_ssd_tests PRIVATE zlong::ssd)
    target_compile_options(zlong_ssd_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_ssd_tests COMMAND zlong_ssd_tests)
endif()
