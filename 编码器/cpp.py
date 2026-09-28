"""Turning one .cpp into a program that links this project.

The Python face of the machine is a module; the C++ face is the libraries themselves. So running
C++ from the editor means compiling and linking against what the project has already built.

**Some files are a program's, and some are on their own.** A file that is one of a project target's
sources -- `宿主/xiangqi/main.cpp`, say -- is compiled the way that target is compiled: with the
target's other sources, its include directories, its definitions and its whole library list. A file
that belongs to no target is a scratch program, built with the project's libraries and nothing else.

**Where that answer comes from depends on where this is running.**

* **包装好的软件**里没有 CMake，没有 MSVC，也没有 `build/`。有的是打包时写下的 `项目清单.json`：
  每个目标的源文件、头文件目录、编译宏、库，以及 Vulkan 的头和导入库在包里的位置。清单里的路径
  是**相对包根**的，所以整包挪到哪台机器上都对。
* **源码树**里没有清单，那就照旧去读 `build/CMakeCache.txt` 和 `build/**/*.vcxproj` —— 那是本机
  CMake 的原话，开发时最准。

不管哪条路，**编译器都只有一个：包里的 Zig**（`运行环境/zig/`，见 `Toolchain`）。它不是系统
环境的一部分，不从 PATH 上找，也不会有第二个候选。

Everything a scratch program needs has to come from the project too, because guessing produces a
program that links and then crashes:

* the **compile definitions**. `ZL_GPU_WITH_VULKAN` is declared PUBLIC and the headers guard members
  with it, so a translation unit that does not have it lays `gpu::Gpu` and `system::System` out
  differently from the libraries and dies on the first call.
* the **Vulkan paths**, which in the package are the copies that shipped with it.
"""

from __future__ import annotations

import json
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path

# Where the two halves of the C++ environment live inside a package. Both are relative to the
# package root, which is also the project root when this is running frozen.
RUNTIME = "运行环境"
ZIG_IN_RUNTIME = "zig"
ZIG_LIBS_IN_RUNTIME = "ziglib"
VULKAN_IN_RUNTIME = "vulkan"
COMPAT_DIR = "工具链"

# What a package carries instead of a build tree. See `Project.from_manifest`.
MANIFEST = "项目清单.json"

# Where a program built from the editor is put. Inside `build/`, because that is the tree this
# machine already treats as disposable.
_OUTPUT = ("build", "编码器")

# The one library in the build tree that must not be linked: it is the import library for the Python
# module, not part of the machine. Name only -- see `_library_name`.
_NOT_LINKED = {"zlong"}

# CMake-said-this-is-not-set markers like `%(AdditionalDependencies)`, which mean "inherit from
# somewhere above" and cannot be resolved here.
_MACRO = "%("

# What a C++ translation unit looks like by its suffix. Only these are gathered when a program
# someone is writing is compiled -- see `Project.sources_for`.
_CXX_SOURCES = (".cpp", ".cc", ".cxx", ".c++")

# The configuration this machine builds. Only Release is read; Debug is not what a Run means.
_CONFIGURATION = "Release|x64"


@dataclass
class Toolchain:
    """The compiler that turns a .cpp into a program.

    **There is one, and it ships with the software.** `运行环境/zig/zig.exe` is Zig -- clang, lld,
    mingw-w64 and libc++ in one binary, MIT-licensed, redistributable. It is not looked for on
    PATH, and nothing else is accepted as a substitute: an editor that falls back to whatever the
    machine happens to have is an editor whose behaviour depends on the machine. See the package's
    `工具链/` for the two shims it needs to compile this project.

    Always returned, even when the compiler is missing: `problem` says what was not there, because
    a caller that gets `None` can only guess.
    """

    compiler: Path | None = None
    compat: Path | None = None             # the shim headers, force-included into every translation
    problem: str = ""

    @property
    def ok(self) -> bool:
        return self.compiler is not None

    @property
    def root(self) -> Path | None:
        """The directory the compiler's own libraries sit in, once it has been found."""
        return self.compiler.parent if self.compiler is not None else None

    @staticmethod
    def find(root: Path) -> "Toolchain":
        """The compiler, which lives at one place and only one: the package's own `运行环境/zig/`.

        `root` is the **package** root -- not whatever folder the editor's file tree is looking at.
        The two are different things and getting them mixed up makes the compiler vanish when the
        user opens a subfolder to read something.
        """
        bundled = root / RUNTIME / ZIG_IN_RUNTIME / "zig.exe"
        if bundled.is_file():
            return Toolchain(compiler=bundled, compat=root / COMPAT_DIR)
        # 说清楚找的是哪一条路径：少了它，"包不完整"和"根认错了"这两种情形长得一模一样。
        return Toolchain(problem=f"没有编译器。找的是这一条：{bundled}\n"
                                 f"编辑器算出来的包根是：{root}\n"
                                 f"编译器和编辑器必须在同一份包里，而且整个包目录要一起用 ——"
                                 f"不要只把 烛龙.exe 拿出来。")


@dataclass
class Project:
    """What a translation unit needs in order to be part of this project.

    `targets` is what the project builds; the flat lists are the fallback for a file that is no
    target's source.
    """

    root: Path
    includes: list[str]
    definitions: list[str]
    libraries: list[str]
    targets: list[Target] = field(default_factory=list)
    # name -> the .a the bundled compiler built. The compiler and these are one set: the object
    # files are GNU ABI and only the same toolchain links them.
    zig_libraries: dict[str, Path] = field(default_factory=dict)
    # The Vulkan import library, when the project was built with Vulkan. `vulkan-1` is linked by
    # path rather than by name, because it is nobody's system library here.
    vulkan: Path | None = None

    @staticmethod
    def discover(root: Path) -> "Project | None":
        """Read the project off disk.

        A package answers with `项目清单.json`; a source checkout answers with what CMake wrote in
        `build/`. The manifest is tried first: in a package the other one is not there at all.
        """
        manifest = root / MANIFEST
        if manifest.is_file():
            found = Project.from_manifest(root, manifest)
            if found is not None:
                return found
        return _from_build_tree(root)

    # -- the manifest, which is what a package answers with ---------------------------------

    @staticmethod
    def from_manifest(root: Path, manifest: Path) -> "Project | None":
        """Read `项目清单.json`.

        Every path in it is **relative to the package root**, so the whole folder can be copied
        anywhere -- which is what shipping one means, and what the old build-tree answer could not
        survive (see `_rehome`).
        """
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        if not isinstance(data, dict):
            return None

        def here(entry: str) -> str:
            return str(root / entry)

        targets = []
        for record in data.get("目标", []):
            sources = [Path(here(name)) for name in record.get("源", [])]
            targets.append(Target(name=record["名字"],
                                  kind=record.get("类型", ""),
                                  sources=sources,
                                  includes=[here(p) for p in record.get("包含", [])],
                                  definitions=list(record.get("定义", [])),
                                  libraries=list(record.get("库", []))))
        vulkan = data.get("vulkan")
        return Project(root=root,
                       includes=[here(p) for p in data.get("包含", [])],
                       definitions=list(data.get("定义", [])),
                       libraries=list(data.get("库", [])),
                       targets=targets,
                       zig_libraries=_index_zig_libraries(root),
                       vulkan=Path(here(vulkan)) if vulkan else None)

    def manifest(self, root: Path) -> dict:
        """This project as JSON, with every path turned into one relative to `root`.

        `root` is the **project** root, not the package directory: a package is a copy of this
        tree's layout, so a path that is `内核/RAM/include` here is `内核/RAM/include` there. The two
        things that do not exist in a source tree -- the bundled Vulkan headers and import library
        -- are given their packaged names by `打包.py` before this is called, and read back under the
        package root by `from_manifest`.

        Written by `打包.py` and read by `from_manifest`; the two are one format and have to be
        changed together.
        """
        def relative(entry: str | Path) -> str | None:
            try:
                return Path(entry).resolve().relative_to(root.resolve()).as_posix()
            except ValueError:
                return None

        def paths(entries: list[str]) -> list[str]:
            return [found for found in (relative(entry) for entry in entries) if found]

        return {
            "格式": 1,
            "包含": paths(self.includes),
            "定义": list(self.definitions),
            "库": list(self.libraries),
            "vulkan": relative(self.vulkan) if self.vulkan is not None else None,
            "目标": [{
                "名字": target.name,
                "类型": target.kind,
                "源": paths([str(source) for source in target.sources]),
                "包含": paths(target.includes),
                "定义": list(target.definitions),
                "库": list(target.libraries),
            } for target in self.targets],
        }

    # -- what a file is ---------------------------------------------------------------------

    def target_for(self, source: Path) -> "Target | None":
        """The project target this file is a source of, if it is one."""
        wanted = source.resolve()
        for target in self.targets:
            for member in target.sources:
                if member.resolve() == wanted:
                    return target
        return None

    def refusal(self, source: Path) -> str:
        """Why this file cannot be run, or empty when it can."""
        target = self.target_for(source)
        if target is not None and not target.runnable:
            return f"{target.name} 是个 {target.kind}，不是程序，没有 main 可以跑"
        return ""

    def output_directory(self, source: Path) -> Path:
        # Named after the target when there is one: two hosts both have a `main.cpp`, and they must
        # not build into the same folder.
        target = self.target_for(source)
        return self.root.joinpath(*_OUTPUT, target.name if target else source.stem)

    def program_for(self, source: Path) -> Path:
        target = self.target_for(source)
        return self.output_directory(source) / ((target.name if target else source.stem) + ".exe")

    def sources_for(self, source: Path) -> list[Path]:
        """Everything that has to be compiled: this file, and whatever else is part of that program.

        A file that belongs to a project target takes the target's list.

        **A file that belongs to no target is a program someone is writing, and a program is more
        than one file.** `main.cpp` beside `rules.cpp` beside `presenter.cpp` is one program; compiling
        only the file that was pressed is how every function the others define comes back as
        `undefined symbol`. So the rule is the folder: **every C++ source beside it comes along.**

        It is the rule the other tools use ("a folder is a program"), and it is the one a person
        already has in their head when they split `main.cpp` in two.
        """
        target = self.target_for(source)
        if target is not None:
            return [member for member in target.sources if member.is_file()]
        beside = sorted(path for path in source.parent.iterdir()
                        if path.is_file() and path.suffix.lower() in _CXX_SOURCES)
        return beside or [source]

    def arguments(self, source: Path, toolchain: Toolchain) -> list[str]:
        """The command line that turns `source` into its program.

        Zig's driver speaks clang: `zig c++`, `-I`, `-o`, and libraries named with `-l` on `-L`.
        """
        out = self.output_directory(source)
        out.mkdir(parents=True, exist_ok=True)
        target = self.target_for(source)

        arguments = ["c++", "-std=c++20", "-O2", "-w"]
        if toolchain.compat is not None:
            # 见 工具链/mingw_compat.h：补的是别人的代码，不是这个项目的。
            #
            # The compat header is force-included here rather than in a wrapper, because there is no
            # wrapper -- the editor starts `zig.exe` directly, so the path never goes through a
            # `.cmd` and a Chinese one is fine.
            arguments += [f"-include{toolchain.compat / 'mingw_compat.h'}",
                          f"-I{toolchain.compat / 'include'}"]
        arguments += [str(path) for path in self.sources_for(source)]
        arguments += [f"-I{path}" for path in (target.includes if target else self.includes)]
        arguments += [f"-D{name}" for name in (target.definitions if target else self.definitions)]
        arguments += ["-o", str(self.program_for(source))]
        arguments += self.link_arguments(target.libraries if target else self.libraries)
        return arguments

    def link_arguments(self, names: list[str]) -> list[str]:
        """Link flags for the names the project lists, in the linker's spelling.

        A name the project built is found as a `.a` beside the compiler -- the libraries and the
        compiler are one set and have to be used together. Anything else is a system library, which
        the GNU toolchain already knows where to find.

        Public because a packager wants the same link line for a program whose output is somewhere
        else than a run's would be. See `packager.py`.
        """
        arguments: list[str] = []
        for name in names:
            stem = _library_name(name)
            if stem == "vulkan-1":
                # The import library is a COFF archive; lld reads it as it is, so it is handed over
                # by path rather than looked for among the compiler's own libraries.
                if self.vulkan is not None:
                    arguments.append(str(self.vulkan))
                continue
            found = self.zig_libraries.get(stem)
            if found is None:
                arguments.append(f"-l{stem}")
            else:
                arguments += [f"-L{found.parent}", f"-l{stem}"]
        return arguments

    def bring_beside(self, program: Path) -> list[Path]:
        """把程序**运行的时候**要找到的那些二进制搬到它旁边。

        清单里那个 `zlong` 不只是一个链接目标 —— 它是 `zlong.dll` 的导入库。包里的整个后端都在那
        一个 DLL 里，而 Windows 的加载器最先搜的就是 exe **自己那一层目录**：放在它旁边，就是全部
        要做的事。

        源码树里没有这一对（那边是一堆 `.a`），所以这里一个都没有的时候什么也不做。
        """
        brought: list[Path] = []
        for library in self.zig_libraries.values():
            name = library.name
            # `libzlong.dll.a` 旁边就是 `zlong.dll`；`libzlong_engine.a` 那种静态库没有配对的 DLL。
            if not (name.startswith("lib") and name.endswith(".dll.a")):
                continue
            binary = library.with_name(name[3:-len(".a")])
            if not binary.is_file():
                continue
            target = program.parent / binary.name
            if not target.is_file() or target.stat().st_mtime < binary.stat().st_mtime:
                shutil.copy2(binary, target)
            brought.append(target)
        return brought

    def is_current(self, source: Path) -> bool:
        """True when the program is already newer than every source and library it uses.

        Running is a deliberate act, but it is also the act you repeat; recompiling unchanged files
        every time would mean watching the compiler print the same lines over and over.
        """
        program = self.program_for(source)
        if not program.is_file():
            return False
        built_at = program.stat().st_mtime

        for member in self.sources_for(source):
            if member.stat().st_mtime > built_at:
                return False
        for library in self.zig_libraries.values():
            if library.stat().st_mtime > built_at:
                return False
        return True


@dataclass
class Target:
    """A program or library the project builds, as CMake configured it.

    Everything here is read out of the generated project file rather than worked out again: the
    sources CMake compiles, the include directories it passes, the definitions it sets, and the full
    library list -- which is where the system libraries a host is allowed to use live (`user32`,
    `gdi32`, `ole32`, `uuid`, and the rest of what CMake links by default).
    """

    name: str
    kind: str                  # Application, StaticLibrary, ...
    sources: list[Path]
    includes: list[str]
    definitions: list[str]
    libraries: list[str]

    @property
    def runnable(self) -> bool:
        return self.kind == "Application"

    @staticmethod
    def read(project_file: Path, root: Path) -> "Target | None":
        """One target out of a generated `.vcxproj`, as CMake wrote it.

        Only used where the build tree is: by a source checkout, and by `打包.py` when it turns that
        tree into the manifest a package carries.
        """
        text = project_file.read_text(encoding="utf-8", errors="replace")
        group = _configuration_group(text)
        if not group:
            return None

        cl = _element(group, "ClCompile")
        link = _element(group, "Link")
        return Target(
            name=project_file.stem,
            kind=_configuration_kind(text),
            # Every path in a generated project file is absolute and was written on the machine that
            # ran CMake, so none of them survive the folder being copied somewhere else. They are
            # put back under this project's root -- see `_rehome`.
            sources=[_rehome(Path(p), root)
                     for p in re.findall(r'<ClCompile Include="([^"]+)"', text)
                     if not p.endswith("CMakeCXXCompilerId.cpp")],
            includes=[str(_rehome(Path(p), root))
                      for p in _split(_element(cl, "AdditionalIncludeDirectories"))],
            # A definition whose value is quoted would have its quotes eaten on the way to the
            # compiler, so they are left out. They are CMake's own bookkeeping (`CMAKE_INTDIR`), not
            # the project's.
            definitions=[d for d in _split(_element(cl, "PreprocessorDefinitions")) if '"' not in d],
            libraries=[_library_name(entry)
                       for entry in _split(_element(link, "AdditionalDependencies"))],
        )


# --- the build tree's answer, which only a source checkout has -------------------------------

# Targets CMake generates for itself; they are not the project's.
_NOT_A_TARGET = {"ALL_BUILD", "ZERO_CHECK", "INSTALL", "RUN_TESTS", "PACKAGE"}


def _from_build_tree(root: Path) -> "Project | None":
    """Read the project out of `build/`, the way a source checkout has to.

    A package never gets here: it has `项目清单.json` instead, and no build tree at all. This is for
    the machine the project is developed on, where CMake's own answer is the most accurate one there
    is -- which is also why `打包.py` reads it to *write* the manifest.
    """
    build = root / "build"
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return None

    def entry(name: str) -> str | None:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(name + ":"):
                return line.split("=", 1)[1].strip()
        return None

    includes = sorted(str(p) for p in (root / "内核").glob("*/include"))
    includes.append(str(root / "引擎" / "include"))
    includes.append(str(root / "external" / "boost"))
    # So a program written for the editor can say `#include "editor_host.h"` from anywhere, rather
    # than counting the `..` up to 宿主/.
    includes.append(str(root / "宿主"))
    # The kernel's headers reach into dynarmic, so a program that only includes
    # zlong/system/system.h still has to resolve <dynarmic/interface/A64/a64.h> and the externals
    # dynarmic drags in behind it.
    includes.append(str(root / "external" / "dynarmic" / "src"))
    includes += sorted(str(p) for p in (root / "external" / "dynarmic" / "externals").glob(
        "*/include"))

    built = sorted(build.glob("**/Release/*.lib"))
    if not built:
        return None
    ours = {_library_name(p.name) for p in built}
    libraries = sorted(ours - _NOT_LINKED)

    definitions = ["NDEBUG"]
    if (entry("ZL_GPU_WITH_VULKAN") or "").upper() in {"ON", "1", "TRUE", "YES"}:
        definitions.append("ZL_GPU_WITH_VULKAN=1")

    vulkan: Path | None = None
    vulkan_include = entry("Vulkan_INCLUDE_DIR")
    if vulkan_include and Path(vulkan_include).is_dir():
        includes.append(vulkan_include)
    vulkan_library = entry("Vulkan_LIBRARY")
    if vulkan_library and Path(vulkan_library).is_file():
        vulkan = Path(vulkan_library)
        libraries.append(_library_name(vulkan.name))

    targets = []
    for project_file in sorted(build.glob("**/*.vcxproj")):
        if project_file.stem in _NOT_A_TARGET:
            continue
        target = Target.read(project_file, root)
        if target is not None and target.sources:
            targets.append(target)

    # The system libraries the project's programs link -- `user32`, `gdi32`, `ole32` and the rest.
    # They are what is left once the ones the project built are taken out: a name whose `.lib` is in
    # `build/` is this project's, everything else is the system's, and the bundled compiler already
    # knows where to find those.
    #
    # **Only what an Application links**, deliberately. This flat list is what a file that is no
    # target's source gets -- a program someone is writing -- so it has to look like what this
    # project's own programs link and nothing else. Widening it to every target drags in the Python
    # module's dependencies, and the first of those is `python313`: the linker is then handed
    # `-lpython313`, which is not a library any link line has ever wanted.
    system = sorted({_library_name(name) for target in targets if target.runnable
                     for name in target.libraries
                     if _library_name(name) not in ours})
    libraries += [name for name in system if name not in libraries]

    includes = [p for p in includes if Path(p).is_dir()]
    return Project(root=root, includes=includes, definitions=definitions, libraries=libraries,
                   targets=targets, zig_libraries=_index_zig_libraries(root), vulkan=vulkan)


def _configuration_group(text: str) -> str:
    """The settings CMake wrote for the configuration that was built."""
    for match in re.finditer(r'<ItemDefinitionGroup Condition="([^"]*)"[^>]*>(.*?)'
                             r"</ItemDefinitionGroup>", text, re.S):
        if _CONFIGURATION in match.group(1):
            return match.group(2)
    return ""


def _configuration_kind(text: str) -> str:
    found = re.search(r'<PropertyGroup[^>]*Label="Configuration"[^>]*>.*?'
                      r"<ConfigurationType>([^<]*)<", text, re.S)
    return found.group(1).strip() if found else ""


def _element(text: str, tag: str) -> str:
    found = re.search(rf"<{tag}[ >](.*?)</{tag}>", text, re.S)
    return found.group(1) if found else ""


def _split(value: str) -> list[str]:
    return [part.strip() for part in value.split(";")
            if part.strip() and not part.strip().startswith(_MACRO)]


def _index_zig_libraries(root: Path) -> dict[str, Path]:
    """The libraries the bundled compiler built, by bare name.

    Two places, because there are two situations: a package ships them under `运行环境/ziglib/`, and
    a source checkout has them in `build-zig/` from whoever last built the project with Zig.

    **A package ships one DLL, not the archives it was folded from.** Its import library is
    `libzlong.dll.a`, and the name the linker wants is `zlong` -- the `lib` and the `.dll` are both
    the filesystem's way of saying "this is the import library for that DLL", and neither belongs on
    a `-l` line.
    """
    found: dict[str, Path] = {}
    for base in (root / RUNTIME / ZIG_LIBS_IN_RUNTIME, root / "build-zig"):
        if not base.is_dir():
            continue
        for library in sorted(base.glob("**/*.a")):
            if "CMakeFiles" in library.parts:
                continue
            name = library.stem[3:] if library.stem.startswith("lib") else library.stem
            if name.endswith(".dll"):
                name = name[:-len(".dll")]
            found.setdefault(name, library)
    return found


def _rehome(path: Path, root: Path) -> Path:
    """A path out of a generated project file, found again under this project's root.

    CMake writes absolute paths, so a build tree that has been copied -- which is what shipping one
    means -- names files that were on somebody else's disk. The tail of the path is what identifies
    the file, so the longest tail that exists under `root` wins: `.../somewhere/宿主/xiangqi/main.cpp`
    becomes `<root>/宿主/xiangqi/main.cpp`.

    The copy under `root` is preferred even when the absolute path still exists. On the machine the
    build tree came from, both are there -- and it is the local one that a run is about, whether
    that is the same file or a copy of it in a release folder.
    """
    parts = path.parts
    for start in range(1, len(parts)):
        candidate = root.joinpath(*parts[start:])
        if candidate.exists():
            return candidate
    return path


def _library_name(entry: str) -> str:
    """A dependency out of a project file, as the linker's `-l` wants it: just the name.

    `.vcxproj` writes dependencies the way MSVC's linker wants them, which is often an absolute path
    to `build/**/Release/foo.lib`. The GNU linker wants a name it can find on a `-L` path, and the
    packaged libraries in `运行环境/ziglib` are found by name, so the path and the `.lib` go.

    Getting this wrong is not a small thing: `-lD:\\...\\foo.lib` is not a library the linker can
    find, and what Zig says about it is `failed to check zig installation for DLL import libs` --
    which points at the compiler, not at the argument.
    """
    return Path(entry).stem
