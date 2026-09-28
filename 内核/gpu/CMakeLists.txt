# The Vulkan render backend is optional on purpose: a machine with only a CPU
# must be able to build and run the software render path without the Vulkan SDK.
# Everything outside vulkan/ is graphics-API-free.
option(ZL_GPU_WITH_VULKAN "Build the Vulkan render backend" ON)

set(ZL_GPU_SOURCES
    src/engine/maxwell3d.cpp
    src/engine/pushbuffer.cpp
    src/gpu.cpp
    src/memory/gpu_address_space.cpp
    src/memory/gpu_memory.cpp
    src/render/mode.cpp
    src/shader/interp.cpp
    src/shader/maxwell.cpp
    src/shader/maxwell_asm.cpp
    src/shader/spirv_emitter.cpp
    src/software/backend.cpp
    src/software/pool.cpp
    src/software/rasterizer.cpp
    src/software/target.cpp
    src/surface/format.cpp
    src/surface/layout.cpp
    src/surface/texel.cpp
)

set(ZL_GPU_TEST_SOURCES
    tests/test_main.cpp
    tests/test_gpu_memory.cpp
    tests/test_maxwell3d.cpp
    tests/test_maxwell_asm.cpp
    tests/test_mode_selection.cpp
    tests/test_pushbuffer.cpp
    tests/test_shader_decode.cpp
    tests/test_shader_interp.cpp
    tests/test_software_render.cpp
    tests/test_submit_pushbuffer.cpp
    tests/test_surface_layout.cpp
    tests/test_texture_format.cpp
)

if (ZL_GPU_WITH_VULKAN)
    find_package(Vulkan REQUIRED)
    list(APPEND ZL_GPU_SOURCES
        src/vulkan/vk_context.cpp
        src/vulkan/vk_format.cpp
        src/vulkan/vk_memory.cpp
        src/vulkan/vk_renderer.cpp
    )
    list(APPEND ZL_GPU_TEST_SOURCES
        tests/test_guest_memory_import.cpp
        tests/test_render_triangle.cpp
        tests/test_vk_smoke.cpp
    )
endif()

add_library(zlong_gpu STATIC ${ZL_GPU_SOURCES})
add_library(zlong::gpu ALIAS zlong_gpu)

target_include_directories(zlong_gpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# zlong::ram brings zlong::cpu (and dynarmic) transitively; the GPU layer needs
# cpu::Core for the park/unpark coordination seam.
target_link_libraries(zlong_gpu PUBLIC zlong::ram)

if (ZL_GPU_WITH_VULKAN)
    target_link_libraries(zlong_gpu PUBLIC Vulkan::Vulkan)
    target_compile_definitions(zlong_gpu PUBLIC ZL_GPU_WITH_VULKAN=1)
endif()

target_compile_features(zlong_gpu PUBLIC cxx_std_20)
target_compile_options(zlong_gpu PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

option(ZL_BUILD_GPU_TESTS "Build the GPU-layer tests" ON)

if (ZL_BUILD_GPU_TESTS)
    add_executable(zlong_gpu_tests ${ZL_GPU_TEST_SOURCES})
    target_include_directories(zlong_gpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(zlong_gpu_tests PRIVATE zlong::gpu)
    target_compile_options(zlong_gpu_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_gpu_tests COMMAND zlong_gpu_tests)
endif()
