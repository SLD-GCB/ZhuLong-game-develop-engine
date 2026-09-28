add_library(zlong_service STATIC
    src/address_space.cpp
    src/boot.cpp
    src/handle.cpp
    src/ipc.cpp
    src/kernel.cpp
    src/registry.cpp
    src/object.cpp
    src/process.cpp
    src/session.cpp
    src/svc.cpp
    src/sync.cpp
    src/services/audout_service.cpp
    src/services/audren_service.cpp
    src/services/debug_service.cpp
    src/services/fsp_service.cpp
    src/services/nvdrv_service.cpp
    src/services/time_service.cpp
    src/thread.cpp
)
add_library(zlong::service ALIAS zlong_service)

target_include_directories(zlong_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Kernel implements cpu::CpuHost and speaks to guest memory through the CPU
# layer's GuestMemory, so it needs cpu::Core -- which comes with zlong::ram.
# It also maps guest syscalls onto Gpu::SubmitPushbuffer, which is what nvdrv is:
# that path is the kernel's, so the GPU layer is a dependency of this one.
# audout hands the guest's samples to an audio::Sink, and audren mixes with the
# audio layer's mixer before doing the same, so the audio layer is a dependency of
# this one the same way the GPU layer is.
target_link_libraries(zlong_service PUBLIC zlong::audio zlong::gpu zlong::mount zlong::ram)

target_compile_features(zlong_service PUBLIC cxx_std_20)
target_compile_options(zlong_service PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

option(ZL_BUILD_SERVICE_TESTS "Build the service-layer tests" ON)

if (ZL_BUILD_SERVICE_TESTS)
    add_executable(zlong_service_tests
        tests/test_main.cpp
        tests/test_audout.cpp
        tests/test_audren.cpp
        tests/test_boot.cpp
        tests/test_fsp.cpp
        tests/test_handles.cpp
        tests/test_ipc.cpp
        tests/test_memory_svc.cpp
        tests/test_nvdrv.cpp
        tests/test_objects.cpp
        tests/test_process.cpp
        tests/test_service.cpp
        tests/test_svc.cpp
        tests/test_sync_svc.cpp
        tests/test_thread_svc.cpp
        tests/test_sync.cpp
    )
    target_include_directories(zlong_service_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(zlong_service_tests PRIVATE zlong::service)
    target_compile_options(zlong_service_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_service_tests COMMAND zlong_service_tests)
endif()
