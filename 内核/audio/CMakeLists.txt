# Above the machine and below a game, like the render layer: the mixer turns a
# description of what is playing into the samples themselves, and a sink decides what
# becomes of them. Nothing here needs a device.
add_library(zlong_audio STATIC
    src/mixer.cpp
    src/wav_sink.cpp
)
add_library(zlong::audio ALIAS zlong_audio)

target_include_directories(zlong_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_features(zlong_audio PUBLIC cxx_std_20)
target_compile_options(zlong_audio PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

option(ZL_BUILD_AUDIO_TESTS "Build the audio-layer tests" ON)

if (ZL_BUILD_AUDIO_TESTS)
    add_executable(zlong_audio_tests
        tests/test_main.cpp
        tests/test_mixer.cpp
        tests/test_wav_sink.cpp
    )
    target_include_directories(zlong_audio_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(zlong_audio_tests PRIVATE zlong::audio)
    target_compile_options(zlong_audio_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_audio_tests COMMAND zlong_audio_tests)
endif()
