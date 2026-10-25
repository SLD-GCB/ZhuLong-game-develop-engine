# 烛龙 (ZhuLong) - host-side programs.
#
# These use the machine, they are not part of it. A program describes content and hands
# over a Platform; 内核/system owns the kernel, the engine and the loop. Keeping them in
# one place is what stops "a program that drives the machine" from drifting into the
# machine.

add_executable(zlong_render_scene render_scene/main.cpp)

target_link_libraries(zlong_render_scene PRIVATE zlong::system)

target_compile_features(zlong_render_scene PRIVATE cxx_std_20)
target_compile_options(zlong_render_scene PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

# 故宫 (gugong) 这个目标已经不在这儿了：`宿主/gugong/` 的源码不在这个树里，而一个指不到源码的
# `add_executable` 会让**整个** CMake 重配失败（不是少一个目标，是全都配不出来）。要把它找回来，
# 把源码放回 `宿主/gugong/` 再把下面这几行加回来。

# The viewer opens a window. Presentation is a platform concern, so it lives in a
# program and not in the machine -- and a window made of GDI is what a software
# renderer can present through with no new dependency.
add_executable(zlong_viewer viewer/main.cpp)

target_link_libraries(zlong_viewer PRIVATE zlong::system)
if (WIN32)
    target_link_libraries(zlong_viewer PRIVATE user32 gdi32)
endif()

target_compile_features(zlong_viewer PRIVATE cxx_std_20)
target_compile_options(zlong_viewer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

# Mixing a scene to a file needs no window and no device, which is the point: what it
# writes can be auditioned, diffed between runs, or both.
add_executable(zlong_mix_scene mix_scene/main.cpp)

target_link_libraries(zlong_mix_scene PRIVATE zlong::system)

# The sound card, as one more sink. Platform code, so it lives with the programs -- the
# same reason the viewer's window does -- and as its own library so that more than one
# program can play into a device without repeating this.
if (WIN32)
    target_link_libraries(zlong_mix_scene PRIVATE zlong_host_audio)
endif()

target_compile_features(zlong_mix_scene PRIVATE cxx_std_20)
target_compile_options(zlong_mix_scene PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

if (WIN32)
    add_library(zlong_host_audio STATIC audio_device/wasapi_sink.cpp)
    target_include_directories(zlong_host_audio
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/audio_device")
    # ole32 for COM, uuid for the device enumerator's class id.
    target_link_libraries(zlong_host_audio PUBLIC zlong::audio ole32 uuid)
    target_compile_features(zlong_host_audio PUBLIC cxx_std_20)
    target_compile_options(zlong_host_audio PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
endif()

# --- 象棋 (Chinese chess) --------------------------------------------------
# A game, so a host program like the others -- but big enough to be split: the rules are
# their own library because they are the part worth testing and they need no GPU, no
# window and no key to be exercised.
add_library(zlong_xiangqi_rules STATIC xiangqi/rules.cpp)
add_library(zlong::xiangqi_rules ALIAS zlong_xiangqi_rules)
target_include_directories(zlong_xiangqi_rules PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/xiangqi")
target_compile_features(zlong_xiangqi_rules PUBLIC cxx_std_20)
target_compile_options(zlong_xiangqi_rules PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

add_executable(zlong_xiangqi xiangqi/main.cpp xiangqi/figures.cpp xiangqi/glyph.cpp
                             xiangqi/presenter.cpp xiangqi/computer.cpp)
target_link_libraries(zlong_xiangqi PRIVATE zlong::xiangqi_rules zlong::system)
target_compile_features(zlong_xiangqi PRIVATE cxx_std_20)
target_compile_options(zlong_xiangqi PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)
if (WIN32)
    # user32 for the keyboard, gdi32 for the window, the DIB and the face's character.
    target_link_libraries(zlong_xiangqi PRIVATE user32 gdi32)
endif()

# Every other part's tests are behind a switch like this one; this block was not, which meant a
# build could not be told to leave the tests out at all -- and the moment their sources are gone it
# is not a choice, it is a configure error ("No SOURCES given to target").
option(ZL_BUILD_XIANGQI_TESTS "Build the xiangqi-host tests" ON)

if (ZL_BUILD_XIANGQI_TESTS)
    add_executable(zlong_xiangqi_tests
        xiangqi/tests/test_main.cpp
        xiangqi/tests/test_computer.cpp
        xiangqi/tests/test_rules.cpp
        xiangqi/computer.cpp
    )
    target_include_directories(zlong_xiangqi_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/xiangqi/tests")
    target_link_libraries(zlong_xiangqi_tests PRIVATE zlong::xiangqi_rules)
    target_compile_options(zlong_xiangqi_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_xiangqi_tests COMMAND zlong_xiangqi_tests)
endif()

# 建模器: the studio's own tool. It draws a model and lets the panel point at it, so it is a
# host like the others -- it implements Platform and hands it over. What it is not is a
# game: it wants Vulkan (require_vulkan) because it is looked at while being used.
add_executable(zlong_modeler modeler/main.cpp)

target_link_libraries(zlong_modeler PRIVATE zlong::system)

target_compile_features(zlong_modeler PRIVATE cxx_std_20)
target_compile_options(zlong_modeler PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

# 音频器: the studio's second tool, the same shape as the modeler -- a Platform host driven by
# the panel -- except that it makes a sound. It wants Vulkan (it is looked at while used) and,
# on Windows, the sound card (zlong_host_audio) so it can be heard while it is edited.
#
# **`zlong_audiotool`，不是 `zlong_audio`** —— 后面那个名字已经是内核那层音频库了（`内核/audio`）。
add_executable(zlong_audiotool audio/main.cpp)

target_link_libraries(zlong_audiotool PRIVATE zlong::system)

if (WIN32)
    target_link_libraries(zlong_audiotool PRIVATE zlong_host_audio)
endif()

target_compile_features(zlong_audiotool PRIVATE cxx_std_20)
target_compile_options(zlong_audiotool PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)
