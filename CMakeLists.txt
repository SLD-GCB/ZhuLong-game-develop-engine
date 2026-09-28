cmake_minimum_required(VERSION 3.20)
project(ZhuLong LANGUAGES C CXX)

# Bundled externals inside dynarmic still declare cmake_minimum_required()
# older than 3.5, which CMake 4.x rejects outright.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

enable_testing()

# --- dynarmic -------------------------------------------------------------
# dynarmic requires Boost *headers* and does not bundle them.
set(BOOST_ROOT "${CMAKE_SOURCE_DIR}/external/boost" CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/boost" CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON)
set(Boost_NO_BOOST_CMAKE ON)

# We only need the AArch64 frontend; dropping A32/Thumb roughly halves the
# compile time.
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)

add_subdirectory(external/dynarmic)

# --- the emulator core ----------------------------------------------------
add_subdirectory(内核/cpu)
add_subdirectory(内核/RAM)
add_subdirectory(内核/SSD)
add_subdirectory(内核/gpu)
add_subdirectory(内核/audio)
add_subdirectory(内核/mount)
add_subdirectory(内核/service)

# --- the engine -----------------------------------------------------------
# Above the machine, below a game: scene, renderer, math, meshes.
add_subdirectory(引擎)

# --- the machine ----------------------------------------------------------
# The kernel at the top. It owns 内核/ and 引擎/ together and drives them, so it comes
# after both; a host program uses this and nothing under it.
add_subdirectory(内核/system)

# --- host-side programs ---------------------------------------------------
# These drive the core; they are not part of it.
add_subdirectory(宿主)
