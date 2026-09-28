# 烛龙 (ZhuLong) - the machine: the kernel at the top, owning the engine and the host.
#
# 内核/ had every part and 引擎/ had the engine, but nobody owned them together. This
# layer is above both: it takes the kernel and the engine as its halves and drives them,
# which is why it depends on zlong::service and zlong::engine and not the other way
# round. A host program uses this and nothing below it.
add_library(zlong_system STATIC
    src/system.cpp
)
add_library(zlong::system ALIAS zlong_system)

target_include_directories(zlong_system PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(zlong_system PUBLIC zlong::engine zlong::service)

target_compile_features(zlong_system PUBLIC cxx_std_20)
target_compile_options(zlong_system PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
)

option(ZL_BUILD_SYSTEM_TESTS "Build the machine-layer tests" ON)

# --- the Python face of the machine ---------------------------------------
#
# The front end is Qt (PySide6) and the backend is this library, so the bridge between them is a
# *binding* of System rather than a protocol: the front end is given the class, not a language to
# speak. Off unless asked for, because it needs Python and pybind11 present and they are not part
# of the project.
option(ZL_BUILD_PYTHON_BINDINGS "Build the Python module for the machine layer" OFF)

if (ZL_BUILD_PYTHON_BINDINGS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)

    # pybind11 is header-only; the interpreter knows where its headers are, so ask it rather than
    # guessing at a CMake package prefix.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import pybind11; print(pybind11.get_include())"
        OUTPUT_VARIABLE ZL_PYBIND11_INCLUDE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ZL_PYBIND11_LOOKUP
    )
    if (NOT ZL_PYBIND11_LOOKUP EQUAL 0)
        message(FATAL_ERROR "pybind11 is not importable by ${Python3_EXECUTABLE}")
    endif()

    Python3_add_library(zlong MODULE python/bindings.cpp WITH_SOABI)
    target_include_directories(zlong PRIVATE "${ZL_PYBIND11_INCLUDE}")
    target_link_libraries(zlong PRIVATE zlong::system)
    target_compile_features(zlong PRIVATE cxx_std_20)
    # /bigobj: the binding file holds a lot of template instantiations and MSVC's default section
    # limit is not enough for them.
    target_compile_options(zlong PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4 /bigobj>
    )

    # Where the module ends up is the packager's business, not this file's: `打包.py` puts it in
    # the bundled interpreter's `site-packages`, so that `import zlong` works with nothing set up.
    # (There used to be a POST_BUILD here copying it to `宿主/studio/`, "next to the studio, so
    # `python main.py` finds it". The studio moved to `编码器/` and that directory was gone, so the
    # copy silently created a *file* called `studio` in `宿主/` -- which then shipped.)
endif()

if (ZL_BUILD_SYSTEM_TESTS)
    add_executable(zlong_system_tests
        tests/test_main.cpp
        tests/test_system.cpp
    )
    target_include_directories(zlong_system_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(zlong_system_tests PRIVATE zlong::system)
    target_compile_options(zlong_system_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /permissive- /W4>
    )
    add_test(NAME zlong_system_tests COMMAND zlong_system_tests)
endif()
