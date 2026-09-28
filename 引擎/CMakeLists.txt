# 烛龙 (ZhuLong) - the engine: what sits between the machine and a game.
#
# 内核/ provides the machine (CPU, memory, GPU submission, audio, services). This layer
# turns a scene into draws, and the same scene into frames. It knows the GPU and audio
# layers; it does not know any guest ISA, any syscalls, or any particular game.
add_library(zlong_engine STATIC
    src/file_io.cpp
    src/image_file.cpp
    src/model_file.cpp
    src/obj_file.cpp
    src/renderer.cpp
    src/scene_file.cpp
    src/sound.cpp
)
add_library(zlong::engine ALIAS zlong_engine)

target_include_directories(zlong_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# PUBLIC: a scene carries PCM and its nodes are transformed in world space, so a
# consumer describing one needs the audio layer's vocabulary and the maths alongside it.
target_link_libraries(zlong_engine PUBLIC zlong::gpu zlong::audio)

target_compile_features(zlong_engine PUBLIC cxx_std_20)
target_compile_options(zlong_engine PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

option(ZL_BUILD_ENGINE_TESTS "Build the engine-layer tests" ON)

if (ZL_BUILD_ENGINE_TESTS)
    add_executable(zlong_engine_tests
        tests/test_main.cpp
        tests/test_mesh.cpp
        tests/test_sound.cpp
    )
    target_include_directories(zlong_engine_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(zlong_engine_tests PRIVATE zlong::engine)
    target_compile_options(zlong_engine_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_engine_tests COMMAND zlong_engine_tests)
endif()
