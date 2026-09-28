add_library(zlong_mount STATIC
    src/archive.cpp
    src/entries.cpp
    src/environment.cpp
    src/mount.cpp
    src/reader.cpp
    src/source.cpp
    src/tree.cpp
    src/zip.cpp
)
add_library(zlong::mount ALIAS zlong_mount)

target_include_directories(zlong_mount PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The mount layer is host-side: it turns sources into read-only trees before any
# guest runs, so it needs the byte-addressable device and nothing else.
target_link_libraries(zlong_mount PUBLIC zlong::ssd)

target_compile_features(zlong_mount PUBLIC cxx_std_20)
target_compile_options(zlong_mount PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

option(ZL_BUILD_MOUNT_TESTS "Build the mount-layer tests" ON)

if (ZL_BUILD_MOUNT_TESTS)
    add_executable(zlong_mount_tests
        tests/test_main.cpp
        tests/test_archive.cpp
        tests/test_entries.cpp
        tests/test_mount_table.cpp
        tests/test_reader.cpp
        tests/test_real_source.cpp
        tests/test_source.cpp
        tests/test_tree.cpp
        tests/test_zip.cpp
    )
    target_include_directories(zlong_mount_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(zlong_mount_tests PRIVATE zlong::mount)
    target_compile_options(zlong_mount_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_mount_tests COMMAND zlong_mount_tests)
endif()
