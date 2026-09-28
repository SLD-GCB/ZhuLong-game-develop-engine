"""归一式打包：一个 exe，一个依赖库，一套运行环境，一个 DLL。

    python 打包.py                装到 发行/烛龙-<日期>/
    python 打包.py --zip          装好之后压成一个 zip
    python 打包.py --into D:/某处  换一个地方装
    python 打包.py --no-exe       不冻 exe（从源码跑编辑器的时候用）
    python 打包.py --no-python    不带跑脚本用的解释器
    python 打包.py --no-cxx       不编 C++（跳掉最慢的一步）

## 包里长什么样

    烛龙.exe                    **所有** Python 代码，冻成一个
    依赖库/                      上面那个 exe 的依赖：PySide6、python313.dll、Qt 插件……
    建模器/建模器.exe             编辑器里「3D 建模」那一页起的那个工具
    运行环境/
        ziglib/zlong.dll         **所有** C++ 代码，收成一个二进制
        python/                  跑用户脚本的解释器（标准库只发字节码）
            harness/             子进程的宿主程序（live.pyc + frames.pyc）
    文档/                        手册

## 两条硬规矩

**一、Python 代码进 exe，依赖进依赖库。** `编码器/` 里十几个 `.py` 互相 `import`，以 `main.py` 为
入口冻成一个 exe。用 PyInstaller 的 **onedir** 而不是 onefile：onefile 每次启动都先把自己解到临时
目录再跑，Qt 那么大的一坨解一遍要几秒，一个每次开都等几秒的编辑器不是编辑器。它的依赖落在
`依赖库/`（PyInstaller 管它叫 contents directory），不跟 exe 混在一起。

**二、C++ 全部是一个 `.dll`；发出去的头里只有签名，没有实现。**

* **实现**：内核八个子系统 + 引擎 + 宿主音频 + 第三方（dynarmic / fmt / mcl / Zydis / Zycore），
  全部编成一个 `运行环境/ziglib/zlong.dll`。头里那些 `inline` 的函数体也一起发射进去。
* **头**：铺进 `include/`，但**我们自己的那些要过一遍刀**（`copy_headers`）—— 函数体挖掉、成员
  初始化列表挖掉、带花括号的常量初值换成 `extern` 声明。发出去的只剩类型、函数签名和常量，正文
  在 DLL 里。切完还会**再切一遍**（第二遍必须一处都切不出来）**并编一遍**，切坏了当场就红。
* **编译器要发。** 它是用这个软件的人的工具（`运行环境/zig/`），少了它编辑器里的「运行 C++」就
  用不了 —— 实测报的是"没有编译器"。它自带的那一万多个 `.c/.cpp/.h` 是 **Zig 自己的**（libc /
  compiler-rt / libc++），不是这个项目的代码。
* **垫片和 Vulkan 也要发**：`工具链/` 是用同一套参数编 C++ 必需的，`运行环境/vulkan/` 是
  `#include <vulkan/vulkan.h>` 必需的。

一句话：**要跑得起来的东西一样不少，但能读出实现的一个都没有。**
"""

from __future__ import annotations

import argparse
import json
import os
import py_compile
import re
import shutil
import stat
import struct
import subprocess
import sys
import time
import zipfile
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent
EDITOR = ROOT / "编码器"

# 包里每一个固定名字，只在这儿写一次。
EXE = "烛龙"
EXE_SUFFIX = ".exe"
DEPS = "依赖库"
RUNTIME = "运行环境"
PYTHON = f"{RUNTIME}/python"
ZIG = f"{RUNTIME}/zig"                 # 编译器住在源码树的这儿；**不进包**
ZIGLIBS = f"{RUNTIME}/ziglib"
COMPAT = "工具链"                       # 让 Zig 编得动这个项目的垫片；**不进包**
ZIG_BUILD = "build-zig"                # 打包时用它自己编出来的构建树，一次性

# 建模器：CMake 目标叫 `zlong_modeler`，装进包之后叫「建模器.exe」，放在「建模器/」下。
MODELER_TARGET = "zlong_modeler"
MODELER = "建模器"
MODELER_DIR = "建模器"

# 软件图标。**源图只有项目根目录那一张 `烛龙.jpg`**，`.ico` 是打包时从它生出来的。
#
# **只有 Python 那一个 exe 带图标**（`烛龙.exe`）—— 它就是这份软件本身。别的 exe 不带。
# 生出来的那份 `.ico` 也留在源码树里（`编码器/icon.ico`），因为从源码跑编辑器的时候要用它。
ICON_SOURCE = "烛龙.jpg"
ICON = f"{EDITOR}/icon.ico"
ICON_SIZES = (256, 128, 64, 48, 32, 24, 16)

# --- 一、C++：全部收成一个 DLL -----------------------------------------------------------------
#
# 后端本来编出来是十几个 `.a`。`.a` 也是一包机器码，但它是**一堆目标文件**，符号、反汇编全在，
# 而且头里那些 `inline` 的函数体只能跟着头走 —— 头是要发出去的，那就是源码。
#
# 所以收成一个 DLL，顺带把头里的实现也一起发射进去：
#
#   * **把 `inline` 挂上 `used` 再编一遍头**。`inline` 的函数"没人用就不发射"，而用过它的那几个
#     `.cpp` 只发射了自己那一份（弱符号）。实测 `Subdivide` / `CornerCut` / `DeleteFace` /
#     `ExtrudeFace` 这四条**不在任何一个 `.a` 里** —— 全项目只有建模器那个 exe 用过它们。
#   * **导出表从 `.a` 自己的符号索引里读**（GNU 归档格式那个名字叫 `/` 的成员），不用 `nm`，也不用
#     往源码里写 `__declspec(dllexport)`。
#   * **只导我们自己的命名空间。** libc++ 和 UCRT 的符号一个都不出去，否则用户程序的
#     `operator new`、`memcpy` 会解析到 DLL 这一份，两套运行时打架。
BACKEND = "zlong"                       # 库名，链接行上那个 `-lzlong`
BACKEND_DLL = f"{BACKEND}.dll"
BACKEND_IMPORT = f"lib{BACKEND}.dll.a"  # 导入库，链的时候要的是它
BACKEND_BUILD = "build-打包"             # 中间产物
EXPORTED_NAMESPACES = ("5zlong", "8dynarmic")
FOLDED_IN = ("dynarmic", "fmt", "mcl", "Zydis", "Zycore")
BACKEND_SYSTEM_LIBS = ("ole32", "uuid", "user32", "gdi32")

# 我们自己写的头：函数要给 DLL 发射符号（`pin_functions`），发出去的那份要过刀（`copy_headers`）。
OUR_HEADER_ROOTS = ("内核/*/include", "引擎/include")
OUR_HEADER_FILES = ("宿主/editor_host.h",)

# 要铺进 `include/` 的全部头：我们自己的 + 第三方的。拍平是安全的，因为每一部分自己的 `include/`
# 底下命名本来就不重叠（`zlong/gpu/…`、`zlong/ram/…`、boost、dynarmic 各以自己开头）。
HEADER_ROOTS = (
    "内核/*/include",
    "引擎/include",
    "external/boost",
    "external/dynarmic/src",
    "external/dynarmic/externals/*/include",
)
HEADER_FILES = ("宿主/editor_host.h",)          # 宿主那一边唯一的公开头
NOT_A_HEADER = (".cpp", ".cc", ".cxx", ".c++", ".c", ".m", ".mm", ".asm",
                ".o", ".obj", ".a", ".lib", ".dll", ".exe", ".py", ".pyc")

# 编私有头之前先铺上的那一层标准库。**有几个头不是自足的** —— 实测
# `内核/gpu/include/zlong/gpu/types.h` 用了 `std::size_t` 却没有 `#include <cstddef>`，平时总是
# 被别的头包着，单独当 TU 编就挂。这是编库时的临时件，不进包。
BACKEND_PRELUDE = ("cstddef", "cstdint", "string", "vector", "map", "set", "array", "optional",
                   "functional", "utility", "memory", "algorithm", "cmath", "chrono", "mutex",
                   "thread", "atomic", "condition_variable", "string_view", "type_traits")

# 测试开关，十个：内核七个 + 引擎 + 机器 + 象棋宿主。
#
# **打包时全关掉，而且必须关。** 包里不带测试，测试源文件也已经不在树里，而这些开关默认是 ON
# 的 —— 它们的 `add_executable` 里写死了 `tests/xxx.cpp`，文件不在，CMake 在**配置阶段**就报
# `No SOURCES given to target`，一个目标都建不出来。
TEST_SWITCHES = (
    "ZL_BUILD_CPU_TESTS", "ZL_BUILD_RAM_TESTS", "ZL_BUILD_SSD_TESTS", "ZL_BUILD_GPU_TESTS",
    "ZL_BUILD_AUDIO_TESTS", "ZL_BUILD_MOUNT_TESTS", "ZL_BUILD_SERVICE_TESTS",
    "ZL_BUILD_SYSTEM_TESTS", "ZL_BUILD_ENGINE_TESTS", "ZL_BUILD_XIANGQI_TESTS",
)

# 象棋和故宫编出来的库不进包：它们是那两个游戏自己的零件，和内核、引擎没关系。
NOT_SHIPPED = ("xiangqi", "gugong")

# --- 二、Python：代码进 exe，依赖进依赖库 ------------------------------------------------------

# 编辑器自己的代码。**这份清单是拿来验收的，不是拿来喂 PyInstaller 的。**
#
# exe 里的模块是 PyInstaller 顺着 `import` 自己收的，列在这儿**不是**为了让它们进去，而是为了
# **查**：新加一个模块却没人 import、被某个 `--exclude-module` 误伤、或者它明明在磁盘上却没进
# 归档 —— 这三种都不会自己吭声，只会等用户按了某个按钮才发现。所以装完之后**真的去读一遍归档**。
EDITOR_MODULES = ("console", "cpp", "editor", "findbar", "frames", "highlighter", "main",
                  "packager", "runtime", "templates", "viewport", "window")
# 第二张表是编辑器模块自己 import 的**标准库** —— 照着 `编码器/*.py` 的 `import` 数出来的。
# `sys` 和 `time` 不在表里：它们是内建模块（编在 `python313.dll` 里），归档里永远找不到，列进来
# 只会每次都报一句假警。
EDITOR_DEPENDENCIES = ("ctypes", "dataclasses", "json", "keyword", "os", "pathlib", "re",
                       "shlex", "shutil", "struct")

# 不带进 exe 的 Python 模块。PySide6 的钩子本来就只收"确实 import 了"的 Qt 模块，这份清单是第二
# 道闸：万一哪个被间接拉了进来，也不进包。
_EXE_EXCLUDES = (
    "PySide6.QtWebEngineCore", "PySide6.QtWebEngineQuick", "PySide6.QtWebEngineWidgets",
    "PySide6.QtWebChannel", "PySide6.QtWebSockets",
    "PySide6.QtQml", "PySide6.QtQuick", "PySide6.QtQuick3D", "PySide6.QtQuickWidgets",
    "PySide6.QtMultimedia", "PySide6.QtMultimediaWidgets", "PySide6.QtSpatialAudio",
    "PySide6.Qt3DCore", "PySide6.Qt3DRender", "PySide6.Qt3DInput", "PySide6.Qt3DAnimation",
    "PySide6.Qt3DExtras", "PySide6.Qt3DLogic",
    "PySide6.QtCharts", "PySide6.QtDataVisualization", "PySide6.QtGraphs",
    "PySide6.QtBluetooth", "PySide6.QtNfc", "PySide6.QtPositioning", "PySide6.QtSerialPort",
    "PySide6.QtSensors", "PySide6.QtRemoteObjects", "PySide6.QtScxml", "PySide6.QtStateMachine",
    "PySide6.QtDesigner", "PySide6.QtHelp", "PySide6.QtUiTools", "PySide6.QtTest",
    "PySide6.QtSql", "PySide6.QtPdf", "PySide6.QtPdfWidgets", "PySide6.QtSvgWidgets",
    "PySide6.QtTextToSpeech", "PySide6.QtHttpServer",
    "tkinter", "test", "pydoc_data",
)

# exe 装好之后还要扔掉的一批，路径相对 `依赖库/`。
#
# PySide6 的钩子把 `PySide6/plugins/` 整个搬了进来，插件又是一条条链子：虚拟键盘那个插件拽进
# Qt6VirtualKeyboard → Qt6Quick → Qt6Qml（十几 MB），TLS 后端拽进一份 OpenSSL（7 MB），PDF 和
# SVG 的图片格式各拽进自己那一个模块。**这些链子断在头上**：编辑器不连网、不显示 PDF、不读 SVG、
# 不要虚拟键盘，也没有一行代码 import 那几个模块。
#
# `opengl32sw.dll`（20 MB，Mesa 的软件 OpenGL）**留着**：驱动进了 Qt 黑名单的机器上，它是唯一还
# 能把窗口画出来的东西。省这 20 MB 去换一台机器开不了，不划算。
_EXE_DROP = (
    "PySide6/plugins/platforminputcontexts",     # 链头
    "PySide6/plugins/tls",                       # 链头
    "PySide6/plugins/networkinformation",        # 链头
    "PySide6/plugins/imageformats/qpdf.dll",
    "PySide6/plugins/imageformats/qsvg.dll",
    "PySide6/plugins/iconengines/qsvgicon.dll",
    "PySide6/Qt6Quick.dll", "PySide6/Qt6Qml.dll", "PySide6/Qt6QmlModels.dll",
    "PySide6/Qt6QmlMeta.dll", "PySide6/Qt6QmlWorkerScript.dll",
    "PySide6/Qt6Pdf.dll", "PySide6/Qt6Svg.dll", "PySide6/Qt6VirtualKeyboard.dll",
    "PySide6/Qt6OpenGL.dll", "PySide6/QtNetwork.pyd", "PySide6/Qt6Network.dll",
    # TLS 插件用的那份 OpenSSL。**带 `-x64` 后缀的那两个才是 Qt 的**；没有后缀的
    # `libcrypto-3.dll` 是 CPython 自己的，`_hashlib` 要它，动不得。
    "libcrypto-3-x64.dll", "libssl-3-x64.dll",
    "PySide6/translations",                      # 编辑器没装 QTranslator，这些 .qm 不会被读
)

# 跑用户脚本用的那套解释器里，标准库不要的。`test/` 一个就占 133 MB；`idlelib` 是它自带的编辑
# 器，`tkinter` 是另一个 GUI 工具箱。
#
# `ensurepip` **留着**：带上它这套运行环境才是能长大的。
_UNUSED_IN_STDLIB = (
    "test", "idlelib", "tkinter", "turtledemo", "pydoc_data", "venv",
    "lib2to3", "turtle.py", "antigravity.py", "__pycache__", "site-packages",
)

# 用户脚本要的依赖包。原样的 PySide6 是 665 MB，其中 `Qt6WebEngineCore.dll` 一个就 205 MB，一个
# QtWidgets 程序一样都用不到。这份清单是编辑器用得到的全部。
_PYSIDE6_FILES = (
    "__init__.py", "_config.py", "_git_pyside_version.py",
    "pyside6.abi3.dll",
    "QtCore.pyd", "QtGui.pyd", "QtWidgets.pyd",
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
    "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_codecvt_ids.dll",
    "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll",
)
# 插件里也只有这几个：`platforms` 是**必须有**的（没有 qwindows.dll 就开不了窗口），`styles` 管
# 控件外观，`imageformats` 管存图。
_PYSIDE6_PLUGINS = ("platforms", "styles", "imageformats", "iconengines", "generic",
                    "platforminputcontexts")

# 子进程的宿主程序。**它们只能躺在磁盘上**（子进程拿它自己的解释器当文件跑），而且发的是字节码。
HARNESS = ("live.py", "frames.py")
HARNESS_DIR = f"{PYTHON}/harness"

# --- 三、发出去的头：只有签名，没有实现 --------------------------------------------------------
#
# 跑代码要的东西一样不少：编译器、头、Vulkan、解释器。**但"要发出去的头"必须过一遍刀** ——
# 我们那些头里 `inline` 的函数体是实打实的实现（`BakeMesh` 怎么算、`CornerCut` 怎么扇面），
# 那就是源码。它们的正文已经编进 `zlong.dll` 了，所以这里发出去的只剩类型、声明和常量。
#
# 第三方那些头（boost / dynarmic / vulkan）原样搬 —— 它们本来就是源码，也不是我们要护的东西。
INCLUDE = "include"
VULKAN = f"{RUNTIME}/vulkan"
MANIFEST = "项目清单.json"

# 切完一刀之后再切一遍，第二遍必须一处都切不出来 —— 那才说明"头里没有实现了"。见 `copy_headers`。


# --- 小工具 -----------------------------------------------------------------------------------


def clear(folder: Path) -> None:
    """删掉旧的输出，等一等还在读它的东西。

    Windows 上刚删过的目录可能还被谁按着（资源管理器、杀毒、上一次没退干净的进程），所以第一次
    删不掉就等半秒重试 —— 直接放弃会得到一个"删了一半"的目录，那比没删更坏。
    """
    if not folder.exists():
        return

    def again(function, path, _problem):
        try:
            os.chmod(path, stat.S_IWRITE)
            function(path)
        except OSError:
            time.sleep(0.5)
            function(path)

    for _attempt in range(3):
        try:
            shutil.rmtree(folder, onerror=again)
            return
        except OSError:
            time.sleep(0.5)
    shutil.rmtree(folder, ignore_errors=True)


def size_of(what: Path) -> str:
    """一个目录或一个文件有多大。**两种都要认** —— 报告里既报目录也报单个 exe，只算目录的话
    单个文件会显示 0 MB（那会让整份报告看着像假的）。"""
    if what.is_file():
        return f"{what.stat().st_size / 1e6:.4g} MB"
    if not what.exists():
        return "0 MB"
    total = sum(path.stat().st_size for path in what.rglob("*") if path.is_file())
    return f"{total / 1e6:.0f} MB"


def copy_tree(source: Path, target: Path, report: list[str], what: str = "",
              ignore_extra: tuple[str, ...] = ()) -> None:
    """把一棵树搬过去，顺手记一行。

    `ignore_extra` 是额外要跳过的名字（`文档/` 里那两个生成参考手册的 `.py` 就是靠它留下的）。
    """
    if not source.is_dir():
        report.append(f"{what or str(target)}  缺：没有 {source}")
        return
    shutil.copytree(source, target, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(*ignore_extra) if ignore_extra else None)
    if what:
        report.append(f"{what}  {size_of(target)}")


def site_packages() -> Path:
    """这套解释器的依赖包在哪。"""
    for candidate in (Path(sys.base_prefix) / "Lib" / "site-packages",
                      Path(sys.prefix) / "Lib" / "site-packages"):
        if candidate.is_dir():
            return candidate
    raise SystemExit("这个解释器里没有 site-packages —— 打包要用装了 PySide6 的那个")


def cache_value(name: str) -> str | None:
    """`build/CMakeCache.txt` 里某一项的值，没有就是 None。"""
    cache = ROOT / "build" / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(f"{name}:"):
            return line.split("=", 1)[1].strip()
    return None


def cmake_verdict(done: subprocess.CompletedProcess) -> list[str]:
    """CMake 说了什么，取真正说了的那几句。

    **不能只看最后一行。** 失败时最后一行往往是一句正常的进度（`-- Configuring done`），真正的错
    在它上面几十行。所以先捞带 `error` 的，连同前后各两行；一条都没捞到时再退回去打最后十五行。
    """
    text = (done.stdout or "") + "\n" + (done.stderr or "")
    lines = text.splitlines()
    interesting = [at for at, line in enumerate(lines)
                   if "error" in line.lower() and "CMake Error" not in line[:11]]
    if interesting:
        wanted: set[int] = set()
        for at in interesting:
            wanted.update(range(max(0, at - 2), min(len(lines), at + 3)))
        return [f"    {lines[at].rstrip()}" for at in sorted(wanted)]
    return [f"    {line.rstrip()}" for line in lines[-15:] if line.strip()]


# --- 一、exe -----------------------------------------------------------------------------------


def build_icons(report: list[str]) -> None:
    """从 `烛龙.jpg` 生那个 `.ico` —— **源图只有那一张，它是生出来的。**

    换了图只要重跑一次打包，不用手工去转。已经比源图新就不动它（那两个 `.ico` 也在源码树里，
    编 C++ 的 CMake 要用）。

    Pillow 不在就跳过：`.ico` 已经在树里，接着用旧的那一份，不能因为少一个画图库就整包打不出来。
    """
    source = ROOT / ICON_SOURCE
    if not source.is_file():
        report.append(f"  图标：没有 {ICON_SOURCE}，用树里现有的 .ico")
        return
    try:
        from PIL import Image
    except ImportError:
        report.append("  图标：这个解释器里没有 Pillow，用树里现有的 .ico")
        return

    target = ROOT / ICON
    if target.is_file() and target.stat().st_mtime >= source.stat().st_mtime:
        return
    image = Image.open(source)
    width, height = image.size
    side = min(width, height)
    image = image.crop(((width - side) // 2, (height - side) // 2,
                        (width + side) // 2, (height + side) // 2))
    if image.mode != "RGBA":
        image = image.convert("RGBA")
    target.parent.mkdir(parents=True, exist_ok=True)
    image.save(target, format="ICO", sizes=[(size, size) for size in ICON_SIZES])
    report.append(f"  图标：{ICON}  {target.stat().st_size / 1024:.0f} KB"
                  f"（{len(ICON_SIZES)} 个尺寸，从 {ICON_SOURCE} 生的）")


def editor_package():
    """`编码器/cpp.py`，当模块用。打包要跟编辑器说同一套项目清单，就用它自己的说法。"""
    if str(EDITOR) not in sys.path:
        sys.path.insert(0, str(EDITOR))
    import cpp                                    # noqa: PLC0415 - 见上：路径要先铺好
    return cpp


def frozen_modules(exe: Path, payload: Path) -> set[str]:
    """一个冻好的 exe 里到底装了哪些 Python 模块。

    翻的是 PyInstaller 挂在 exe 尾巴上的归档，三层合起来：

    * **外层**（`CArchiveReader`）—— 二进制、`.pyd`、数据文件，还有里面的那一层 `PYZ`；
    * **`PYZ`**（`open_embedded_archive`）—— 真正的模块名在这儿；
    * **`base_library.zip`** —— 少数基础模块在这儿。

    **这是验收，不是猜。**
    """
    from PyInstaller.archive.readers import CArchiveReader

    names: set[str] = set()
    outer = CArchiveReader(str(exe))
    names |= set(outer.toc)
    for entry in list(outer.toc):
        if entry.endswith(".pyz"):
            names |= set(outer.open_embedded_archive(entry).toc)
    library = payload / "base_library.zip"
    if library.is_file():
        with zipfile.ZipFile(library) as bundle:
            names |= set(bundle.namelist())

    # **模块名要从路径的第一段取，不能拿文件名。** 包存的是 `re/__init__.pyc`，取 stem 得到的是
    # `__init__`，于是 `re` 被报成"缺了" —— 那是这个函数的错，不是包的错（实测踩过）。
    def top(name: str) -> str:
        return Path(name.replace("\\", "/")).parts[0].split(".")[0]

    return {top(name) for name in names}


def build_exe(package: Path, report: list[str]) -> bool:
    """把 `编码器/` 里测试之外的全部 Python 代码冻成一个 exe。"""
    entry = EDITOR / "main.py"
    if not entry.is_file():
        report.append(f"  exe：没有 {entry.relative_to(ROOT)}")
        return False
    try:
        import PyInstaller                         # noqa: F401
    except ImportError:
        report.append("  exe：这个解释器里没装 PyInstaller（pip install pyinstaller）")
        return False

    staging = ROOT / "build-打包"
    clear(staging)
    dist = staging / "exe"
    dist.mkdir(parents=True)

    made = subprocess.run([
        sys.executable, "-m", "PyInstaller",
        "--noconfirm", "--clean",
        "--name", EXE,
        # 没有控制台窗口：这个程序的输出在自己下面那个「输出」面板里，不在黑窗口里。
        "--windowed",
        "--contents-directory", DEPS,
        "--distpath", str(dist),
        "--workpath", str(staging / "work"),
        "--specpath", str(staging),
        # `编码器/` 里是 `import cpp`、`from console import Console` 这种互相找，入口所在的目录
        # 要在 import 路径上，PyInstaller 才知道顺着哪儿把这一棵收完。
        "--paths", str(EDITOR),
        # exe 自己的图标（任务栏、资源管理器看到的那个）。
        *(["--icon", str(EDITOR / "icon.ico")] if (EDITOR / "icon.ico").is_file() else []),
        # 同一个 `.ico` 再当数据带一份：**窗口标题栏上那个**要 Qt 自己设，见 `main.py`。
        *(["--add-data", f"{EDITOR / 'icon.ico'}{os.pathsep}."]
          if (EDITOR / "icon.ico").is_file() else []),
        *(f"--exclude-module={name}" for name in _EXE_EXCLUDES),
        str(entry),
    ], cwd=str(ROOT), capture_output=True, encoding="mbcs", errors="replace", timeout=1800)

    produced = dist / EXE
    if made.returncode != 0 or not (produced / (EXE + EXE_SUFFIX)).is_file():
        report.append("  exe：打包失败")
        for line in ((made.stdout or "") + "\n" + (made.stderr or "")).splitlines():
            if "Error" in line or "error:" in line:
                report.append("    " + line.strip())
                break
        return False

    # PyInstaller 只能往一个空目录里产，所以产在 staging 里再搬过来。
    for child in sorted(produced.iterdir()):
        shutil.move(str(child), str(package / child.name))

    payload = package / DEPS
    dropped = 0
    for name in _EXE_DROP:
        target = payload / name
        if target.is_dir():
            dropped += sum(f.stat().st_size for f in target.rglob("*") if f.is_file())
            shutil.rmtree(target)
        elif target.is_file():
            dropped += target.stat().st_size
            target.unlink()

    clear(staging)

    # **验收：真的去读一遍归档。** 那份清单在磁盘上核不出一件事 —— 模块是不是真的进了 exe，只有
    # exe 自己知道。少一个也不会自己吭声，只会等用户按了某个按钮才炸。
    try:
        installed = frozen_modules(package / (EXE + EXE_SUFFIX), payload)
        wanted = EDITOR_MODULES + EDITOR_DEPENDENCIES
        missing = [name for name in wanted if name not in installed]
    except Exception as problem:                      # noqa: BLE001 - 验不了要说出来，不能装作验过
        report.append(f"    ！归档没读成，这一条没验：{problem}")
    else:
        if missing:
            report.append(f"    ！exe 里少了这些：{'、'.join(missing)}")
        else:
            report.append(f"    {len(EDITOR_MODULES)} 个编辑器模块 + "
                          f"{len(EDITOR_DEPENDENCIES)} 个依赖，都在 exe 里（读了归档，不是猜的）")

    report.append(f"  {EXE}{EXE_SUFFIX}  {size_of(package / (EXE + EXE_SUFFIX))}"
                  f" —— 编码器/ 里测试之外的全部代码")
    report.append(f"  {DEPS}/  {size_of(payload)}（另扔掉 {dropped / 1e6:.0f} MB 用不到的："
                  f"Qt 的 QML / PDF / SVG / 虚拟键盘 / OpenSSL / 翻译，都是插件链拽进来的）")
    return True


# --- 二、运行环境里的 Python -------------------------------------------------------------------


def build_python(package: Path, report: list[str]) -> None:
    """一套自带解释器：解释器、标准库、全部依赖包。"""
    root = Path(sys.base_prefix)
    libraries = site_packages()
    runtime = package / PYTHON
    library = runtime / "Lib"
    library.mkdir(parents=True)

    # 一、解释器本身，加上它要的 MSVC 运行库（可再分发）。
    version = f"{sys.version_info.major}{sys.version_info.minor}"
    for name in ("python.exe", "python3.dll", f"python{version}.dll",
                 "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt"):
        source = root / name
        if source.is_file():
            shutil.copy2(source, runtime / name)
    copy_tree(root / "DLLs", runtime / "DLLs", report)

    # 二、标准库，去掉这套东西用不到的。
    copied = 0
    dropped = 0
    for entry in sorted((root / "Lib").iterdir()):
        if entry.name == "site-packages":
            continue                       # 它不是"不要"，是另装一份（见下）
        if entry.name in _UNUSED_IN_STDLIB:
            if entry.is_dir():
                dropped += sum(f.stat().st_size for f in entry.rglob("*") if f.is_file())
            elif entry.is_file():
                dropped += entry.stat().st_size
            continue
        target = library / entry.name
        if entry.is_dir():
            copy_tree(entry, target, report)
        else:
            shutil.copy2(entry, target)
        copied += 1

    # 三、依赖包：用户脚本要的东西，一个不少。
    installed = library / "site-packages"
    installed.mkdir(parents=True)
    kept = 0
    pyside = libraries / "PySide6"
    if not pyside.is_dir():
        report.append("    缺：PySide6 —— 这个解释器里没装（用户脚本就开不了窗口）")
    else:
        target = installed / "PySide6"
        (target / "plugins").mkdir(parents=True)
        for name in _PYSIDE6_FILES:
            source = pyside / name
            if source.is_file():
                shutil.copy2(source, target / name)
                kept += source.stat().st_size
        for plugin in _PYSIDE6_PLUGINS:
            copy_tree(pyside / "plugins" / plugin, target / "plugins" / plugin, report)
            kept += sum(f.stat().st_size for f in (pyside / "plugins" / plugin).rglob("*")
                        if f.is_file())
        whole = sum(f.stat().st_size for f in pyside.rglob("*") if f.is_file())
        report.append(f"  PySide6 裁到 {kept / 1e6:.0f} MB（原样 {whole / 1e6:.0f} MB）")
    copy_tree(libraries / "shiboken6", installed / "shiboken6", report)
    # **shiboken6/include/ 不要。** 那是"自己写一个 PySide6 绑定"时用的 C++ 头，运行期一个字都用
    # 不到 —— 而包里不许有源码。
    dev_headers = installed / "shiboken6" / "include"
    if dev_headers.is_dir():
        shutil.rmtree(dev_headers)

    # 四、机器在 Python 那一面的门。放在 site-packages 里，于是 `import zlong` 什么都不用设。
    #
    # **用包里那个编译器编出来的那一份**，不是开发机 `build/` 里的：它的导入表里没有 MSVCP140 /
    # VCRUNTIME140（libc++ 和 C++ 运行库是静态链进去的），而 MSVC 那一份要，且加载器不会去
    # `site-packages/PySide6/` 底下找。
    module = zlong_module()
    if module is None:
        report.append("    缺：zlong 模块 —— C++ 那一步没编出来，Python 脚本 import 不到机器")
    else:
        shutil.copy2(module, installed / module.name)
    report.append(f"  {PYTHON}/  {size_of(runtime)}"
                  f"（解释器 Python {sys.version_info.major}.{sys.version_info.minor}；"
                  f"标准库 {copied} 项，去掉约 {dropped / 1e6:.0f} MB 用不到的；"
                  f"zlong 模块 {module.name if module else '没有'}）")


def zlong_module() -> Path | None:
    """机器的 Python 那一面，在哪。

    `build-zig/` 优先：那是用包里那个编译器编的。回落到 `build/` 是给一个还没用 Zig 构建过的源码
    树 —— 那份是 MSVC 编的，要 `MSVCP140.dll`，装进包之前得先想清楚它从哪儿加载。
    """
    for base in (ROOT / ZIG_BUILD, ROOT / "build"):
        for module in sorted(base.glob("**/zlong*.pyd")):
            return module
    return None


def compile_python(package: Path, report: list[str]) -> None:
    """包里所有 `.py` 就地编成 `.pyc`，然后把 `.py` 删掉。

    **CPython 认得没有 `.py` 的 `.pyc`** —— 只要它跟模块放同一层、不在 `__pycache__/` 里，
    `import` 照样成立。所以 `Lib/json/__init__.py` 变成 `Lib/json/__init__.pyc` 之后 `import json`
    还是好的。（标准库里有几个模块是冻在 `python313.dll` 里的，那些本来就没有 `.py`。）

    `.pyi`（类型存根）直接删 —— 那是给编辑器看的，运行期一个字都用不到。
    """
    runtime = package / PYTHON
    if not runtime.is_dir():
        return
    compiled = 0
    dropped = 0
    stuck: list[str] = []
    for path in sorted(runtime.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix == ".pyi":
            path.unlink()
            dropped += 1
            continue
        if path.suffix != ".py":
            continue
        try:
            py_compile.compile(str(path), cfile=str(path.with_suffix(".pyc")), doraise=True)
        except py_compile.PyCompileError:
            stuck.append(path.relative_to(runtime).as_posix())
            continue
        path.unlink()
        compiled += 1
    for cache in sorted(runtime.rglob("__pycache__")):
        if cache.is_dir():
            shutil.rmtree(cache)
    report.append(f"  {PYTHON}/  标准库编成字节码：{compiled} 个 .py -> .pyc"
                  f"（删掉 {dropped} 个 .pyi、清掉 __pycache__）")
    if stuck:
        report.append(f"    ！这些编不了，只能留着源码：{'、'.join(stuck[:5])}")


# --- 三、C++ -----------------------------------------------------------------------------------


def zig_wrappers(zig: Path, where: Path) -> dict[str, Path]:
    """CMake 要的是一个可执行文件，不是 `zig cc`，所以每个子命令配一行包装。

    **这些文件里的路径来自环境变量，不是写在文件里。** cmd 按 OEM 代码页读 `.cmd`，写进文件的中文
    路径会被读成别的字 —— 而这个项目和编译器都可能住在中文路径下。`%ZL_ZIG%` 是运行时从进程环境里
    展开的，Python 放进去的是 Unicode。
    """
    where.mkdir(parents=True, exist_ok=True)
    made: dict[str, Path] = {}
    for name, subcommand, extra in (
            ("cc", "cc", ""),
            ("cxx", "c++",
             ' -include "%ZL_COMPAT%\\mingw_compat.h" -I"%ZL_COMPAT%\\include"'),
            ("ar", "ar", ""),
            ("ranlib", "ranlib", "")):
        wrapper = where / f"zig-{name}.cmd"
        wrapper.write_text(f'@echo off\n"%ZL_ZIG%" {subcommand}{extra} %*\n', encoding="mbcs")
        made[name] = wrapper
    return made


def our_headers() -> list[Path]:
    """我们自己写的那八十来个头 —— 要挂 `used` 才能保证每一条都被发射出来的就是这些。"""
    found: list[Path] = []
    for pattern in OUR_HEADER_ROOTS:
        for root in sorted(ROOT.glob(pattern)):
            found += sorted(path for path in root.rglob("*.h") if path.is_file())
    found += [ROOT / name for name in OUR_HEADER_FILES if (ROOT / name).is_file()]
    return found


def mask_away(text: str) -> str:
    """把注释、字符串、字符字面量换成**等长空白**，之后只剩括号和大括号要看。

    偏移量一个没变，所以切原文的时候直接拿它算出来的位置就行。
    """
    blank = list(text)
    at, length = 0, len(text)
    while at < length:
        here = text[at]
        if here == "/" and text.startswith("//", at):
            end = text.find("\n", at)
            end = length if end < 0 else end
        elif here == "/" and text.startswith("/*", at):
            end = text.find("*/", at + 2)
            end = length if end < 0 else end + 2
        elif here == "'" and 0 < at < length - 1 \
                and text[at - 1].isalnum() and text[at + 1].isalnum():
            # **`0x410F'D070ULL` 里的那一个是数字分隔符，不是字符字面量的开始。** 认错了会去找一个
            # 永远不来的收尾引号，把它后面**整个文件**涂白 —— 实测 `内核/RAM/include/zlong/ram/
            # registers.h` 就是这么整份漏切的（17 个函数体一个没动）。判据：前后都是字母数字。
            at += 1
            continue
        elif here in "\"'":
            end = at + 1
            while end < length and text[end] != here:
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, length)
        else:
            at += 1
            continue
        blank[at:end] = ["\n" if ch == "\n" else " " for ch in text[at:end]]
        at = end
    return "".join(blank)


# 三种大括号，认的是它**前面那段"领头"文字**：
#   * **函数体** —— 领头里有 `()`，而且 `(` 前面那个名字不是 `if` / `for` / `while` 那种
#   * **变量初值** —— 领头以 `=` 收尾（`inline constexpr X a[] = { … };`），值整段换成 `extern` 声明
#   * **别的一律是作用域** —— `class` / `struct` / `enum` / `namespace` / `extern "C"`，不动它
_CONTROL = frozenset((
    "if", "for", "while", "switch", "catch", "sizeof", "alignof", "decltype", "return", "new",
    "delete", "throw", "noexcept", "and", "or", "not", "co_await", "co_return", "co_yield",
))
_TRAILING_NAME = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*$")
_OPERATOR = re.compile(r"\boperator\s*[^\s\w]*\s*$")     # `operator<`、`operator-`、`operator==`
_LEADING_SPECIFIERS = re.compile(r"^(?:\s*(?:inline|constexpr|static|extern|const|volatile))+")
_CONSTANT_WORDS = ("constexpr", "const")
_ACCESS = ("public", "private", "protected")


def closing_brace(masked: str, opening: int) -> int:
    """和 `masked[opening]` 那个 `{` 配对的那个 `}`，没有就 -1。"""
    depth = 0
    for at in range(opening, len(masked)):
        if masked[at] == "{":
            depth += 1
        elif masked[at] == "}":
            depth -= 1
            if depth == 0:
                return at
    return -1


def next_significant(masked: str, at: int) -> int:
    while at < len(masked) and masked[at].isspace():
        at += 1
    return at if at < len(masked) else -1


def top_level_colon(masked: str, start: int, end: int) -> int:
    """[start,end) 里那个成员初始化列表的 `:`，没有就 -1。两条都得守，都是实测踩出来的：

    * **`::` 要一次跳两个。** 只跳第一个的话第二个会被当成初始化列表的开头，`std::uint64_t Foo()
      { … }` 就被切在 `std::` 那里，切出来一句 `inline std:;`。
    * **要有个 `)` 在它前面。** 初始化列表紧跟参数表；反过来 `public:` 的冒号前面没有 `)`（主循环
      会把它当分界，见 `_ACCESS`）。
    """
    if end <= start:
        return -1
    depth = 0
    at = start
    while at < end:
        char = masked[at]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == ":" and depth == 0:
            if masked.startswith("::", at):
                at += 2
                continue
            return at if masked.rfind(")", start, at) >= 0 else -1
        at += 1
    return -1


def after_template(stripped: str) -> str:
    """`template <…>` 之后剩下的那一段；不是模板就原样返回。"""
    if not stripped.startswith("template"):
        return stripped
    at = stripped.find("<")
    if at < 0:
        return stripped
    depth = 0
    while at < len(stripped):
        if stripped[at] == "<":
            depth += 1
        elif stripped[at] == ">":
            depth -= 1
            if depth == 0:
                return stripped[at + 1:].lstrip()
        at += 1
    return stripped


def opener_kind(leader: str) -> str:
    """这个大括号开的是"函数体"、"变量初值"、"模板"，还是"作用域"。"""
    stripped = leader.strip()
    if not stripped:
        return "scope"
    # **模板单独一类：不切、也不钉。** 模板的体必须在头里 —— 用户实例化它的时候要看得到定义，
     # 挖走就编不出用户的程序来。这是 C++ 的规定，和 `constexpr` 常量一样绕不开。实测有 4 处，
     # 在三个头里（`ram/ram.h`、`service/handle.h`、`service/object.h`）。
    rest = after_template(stripped)
    if rest is not stripped:
        if rest.startswith(("struct", "class", "union", "enum")):
            return "scope"                # `template <…> struct X { … };` 是作用域
        return "template" if "(" in rest else "scope"
    if stripped.endswith("="):
        return "variable"
    if "(" not in stripped:
        return "scope"                    # class / struct / enum / namespace / extern "C"
    # **取第一个 `(`，不是最后一个。** 参数表是这段文字里第一个括号；最后一个往往是成员初始化
    # 列表里的某个调用（`MemoryBlockDevice(std::uint64_t size) : data_(static_cast<…>(size), 0)`），
    # 拿它当参数表，前面剩下来的就成了 `…static_cast<std::size_t>`，名字取不到，整个构造函数漏切
    # （实测 `ssd/block_device.h` 就是这么漏的）。
    before = stripped[:stripped.find("(")].rstrip()
    name = _TRAILING_NAME.search(before)
    if name is not None:
        return "scope" if name.group(1) in _CONTROL else "function"
    # **`operator<` / `operator-` / `operator*` 这种末尾是符号的。** "取最后一个词"在它们身上取
    # 不到东西，会被当成作用域、整个漏切（实测 `mesh.h` 的 `operator<` 和 `math.h` 的 `operator-`
    # 就是这么漏的）。
    return "function" if _OPERATOR.search(before) else "scope"


def body_group(masked: str, first: int) -> tuple[int, int]:
    """一条函数声明的体是它**最后一个**顶层大括号组。

    为什么不是第一个：带成员初始化列表的构造函数长这样 ——

        Sender(std::FILE* out) : state_{new State}, ready_{false} { … }

    那些 `{…}` 先出现，它们不是体。体后面跟的是下一条声明（不是 `,`、不是 `{`），所以一直往后走，
    走到"后面不再接 `,` 或 `{`"为止，那一组才是体。
    """
    opening = first
    close = closing_brace(masked, opening)
    while close >= 0:
        following = next_significant(masked, close + 1)
        if following < 0 or masked[following] not in "{,":
            break
        if masked[following] == ",":
            following = next_significant(masked, following + 1)
            if following < 0 or masked[following] != "{":
                break
        opening = following
        close = closing_brace(masked, opening)
    return opening, close


def _functions(text: str) -> list[tuple[int, int, int, str, str]]:
    """走一遍，产出每一处"要切 / 要钉"的地方。

    每项是 **(声明起点, 处理起点, 处理终点, 种类, 声明文字)**，种类是 `"function"` 或 `"variable"`。

    **切头（`strip_bodies`）和钉符号（`pin_functions`）都走这一遍。** 同一个扫描两处用，"头里挖掉
    的"和"库里钉住的"才一定是同一批函数。

    这两半对不上是什么下场，实测过：`Camera::View()` / `Node::Drawable()` / `System::ok()` 这些
    **写在类里面的成员函数是隐式 inline，根本没有 `inline` 这个关键字**，原来的做法只钉了带关键
    字的那些，于是用户一编就是一片 `undefined symbol`（头里只剩声明，库里没有符号）。
    """
    masked = mask_away(text)
    found: list[tuple[int, int, int, str, str]] = []
    scopes: list[str] = []                # 每个打开的 `{` 是哪一种
    statement = 0
    at = 0
    while at < len(masked):
        char = masked[at]
        if char == ";":
            statement = at + 1
            at += 1
        elif char == "}":
            if scopes:
                scopes.pop()
            statement = at + 1
            at += 1
        elif char == ":":
            # `public:` 也是一条分界，不然下一条声明的"领头"会从它前面开始算 —— 实测就是这么把
            # 成员切掉、切出一句 `public;` 的。`::` 不是分界，跳过。
            if masked.startswith("::", at):
                at += 2
            elif masked[statement:at].strip() in _ACCESS:
                statement = at + 1
                at += 1
            else:
                at += 1
        elif char == "#" and (at == 0 or masked[at - 1] == "\n"):
            # **整行预处理器指令跳过，并把它当成分界。** 不跳的话 `#if defined(_WIN32)` 里那个
            # `(` 会让后面第一个 `{` 被认成函数体 —— 实测 `editor_host.h` 整个文件被这么一口吞掉
            # 过（480 行切完只剩 49 行）。
            line = masked.find("\n", at)
            while line > 0 and masked[line - 1] == "\\":
                line = masked.find("\n", line + 1)
            if line < 0:
                break
            statement = line + 1
            at = line + 1
        elif char != "{":
            at += 1
        else:
            leader = masked[statement:at]
            kind = opener_kind(leader)
            if kind in ("function", "template"):
                opening, close = body_group(masked, at)
                if close < 0:
                    at += 1
                    continue
                if kind == "template":
                    found.append((statement, opening, close + 1, "template", ""))
                else:
                    colon = top_level_colon(masked, statement, opening)
                    start = colon if colon >= 0 else opening
                    found.append((statement, start, close + 1, "function", ""))
                statement = close + 1
                at = close + 1
            elif kind == "variable":
                close = closing_brace(masked, at)
                if close < 0:
                    at += 1
                    continue
                equal = masked.rindex("=", statement, at)
                if any(scope in ("class", "struct", "union") for scope in scopes):
                    # 类里的默认成员初值：它跟着类定义一起被看见，挖掉会变成"另一个类"。
                    at = close + 1
                    continue
                declaration = masked[statement:equal].strip()
                found.append((statement, equal, close + 1, "variable", declaration))
                statement = close + 1
                at = close + 1
            else:
                scopes.append("class" if leader.strip().startswith(("class", "struct", "union"))
                              else "other")
                statement = at + 1
                at += 1
    return found


def strip_bodies(text: str) -> tuple[str, int]:
    """把一份头里**每一处实现**挖掉，只留类型、声明和常量。返回（切完的文本，切了几处）。

    函数的体整段跳过 —— 体里面的东西（lambda、if、局部变量）就再也不用管了。切漏没切漏由
    `copy_headers` 用另一套笨办法查（见 `leftover_bodies`）。
    """
    cuts: list[tuple[int, int, str]] = []
    for statement, start, end, kind, declaration in _functions(text):
        if kind == "template":
            continue                      # 模板的体必须留在头里，见 `opener_kind`
        if kind == "function":
            cuts.append((start, end, ";"))
            continue
        # 带花括号的常量初值：整段换成 `extern` 声明（`constexpr` 落成 `const` ——
        # `extern constexpr` 不合法）。
        specifiers = _LEADING_SPECIFIERS.match(declaration)
        words = specifiers.group(0) if specifiers else ""
        rest = declaration[len(words):]
        kept = "extern const " if any(word in words for word in _CONSTANT_WORDS) else "extern "
        cuts.append((statement, start, kept + rest + " "))
        cuts.append((start, end, ";"))

    pieces: list[str] = []
    cursor = 0
    for start, end, replacement in sorted(cuts):
        if start < cursor:
            continue                      # 交叠了（理论上不该有）：先来的算
        pieces.append(text[cursor:start])
        pieces.append(replacement)
        cursor = end
    pieces.append(text[cursor:])
    return "".join(pieces), len(cuts)


def pin_functions(text: str) -> tuple[str, int]:
    """在每个函数声明前面插一句 `__attribute__((used))`，返回（文本，钉了几个）。

    **为什么要钉。** `inline` 的函数"没人用就不发射"，而写过它的那几个 `.cpp` 只发射了自己那一份
    （弱符号）。头里一旦只剩声明，用户程序就得从 `zlong.dll` 拿这些符号 —— 库里没有就是一片
    undefined symbol。钉上 `used`，编译器就会真的把它发射出来。

    **为什么不能只钉带 `inline` 关键字的**（原来就是那么干的，翻过车）：写在类里面的成员函数是
    **隐式 inline**，没有那个关键字，于是 `Camera::View()`、`Node::Drawable()`、`System::ok()` 这批
    全漏了。现在走的是 `_functions` 那同一个扫描 —— 头里切掉几处，库里就钉几处。

    钉完还是**弱符号**，和库里已经有的那些同名弱符号合得起来，不会撞（去掉 `inline` 变强定义就会
    撞 —— 那也试过，lld 报 duplicate symbol）。
    """
    pieces: list[str] = []
    at = 0
    pinned = 0
    for statement, _start, _end, kind, _declaration in _functions(text):
        if kind != "function":
            continue
        pieces.append(text[at:statement])
        pieces.append("__attribute__((used)) ")
        at = statement
        pinned += 1
    pieces.append(text[at:])
    return "".join(pieces), pinned


def archive_symbols(path: Path) -> list[str]:
    """一张 `.a` 里**定义出来了的**全局符号。

    GNU 的归档格式自己在头里放一张索引 —— 名字是 `/` 的那个成员：四个字节的条数、那么多个四字节
    的偏移，然后一串以 NUL 分隔的名字。所以不用 `nm`，也不用往源码里写 `__declspec(dllexport)`。
    """
    data = path.read_bytes()
    if data[:8] != b"!<arch>\n":
        return []
    at = 8
    while at + 60 <= len(data):
        head = data[at:at + 60]
        try:
            size = int(head[48:58].decode().strip())
        except ValueError:
            return []
        body = data[at + 60:at + 60 + size]
        name = head[:16].decode("latin-1").strip()
        if name == "/":
            count = struct.unpack(">I", body[:4])[0]
            return [item.decode("utf-8", "replace")
                    for item in body[4 + 4 * count:].split(b"\0") if item]
        if name == "/SYM64/":
            count = struct.unpack(">Q", body[:8])[0]
            return [item.decode("utf-8", "replace")
                    for item in body[8 + 8 * count:].split(b"\0") if item]
        at += 60 + size + (size & 1)
    return []


def compile_flags(build: Path) -> list[str] | None:
    """编私有头要用的那些旗子，从**机器那一层**的源文件上取。

    **问构建树要，不自己拼。** 自己拼就是手抄一份 `-I` 清单，而某个子系统少一个 `-I` 的那种错，
    要到编的时候才发作；`compile_commands.json` 是 CMake 自己写下来的真话。

    取的是 `system.cpp` 那一份 —— `zlong::system` 底下是**所有**子系统，所以它的 `-I` 是全集。
    拿引擎那一条来取会在 `内核/mount` 那里少一个 `-I`（实测：`archive.h` 引 `ssd/block_device.h`
    就找不着了）。
    """
    listing = build / "compile_commands.json"
    if not listing.is_file():
        return None
    for entry in json.loads(listing.read_text(encoding="utf-8")):
        if not entry.get("file", "").replace("\\", "/").endswith("内核/system/src/system.cpp"):
            continue
        words = list(entry["arguments"]) if "arguments" in entry \
            else entry.get("command", "").split()
        kept: list[str] = []
        skip = False
        for word in words:
            if skip:
                skip = False
                continue
            if word in ("-o", "-c", "-MF", "-MT", "-MQ"):
                skip = True
                continue
            if word == "-include":
                kept.append(word)
            elif word.startswith(("-I", "-D", "-std=", "-f", "-m", "-W", "-O", "-g")):
                kept.append(word)
        return kept
    return None


def build_dll(package: Path, report: list[str], zig: Path, environment: dict, build: Path,
              libraries: list[Path]) -> bool:
    """把整个 C++ 后端收成**一个** `zlong.dll`，铺进 `ziglib/`。"""
    staging = ROOT / BACKEND_BUILD
    clear(staging)
    staging.mkdir(parents=True, exist_ok=True)

    flags = compile_flags(build)
    if flags is None:
        report.append(f"    后端：没有 {ZIG_BUILD}/compile_commands.json，编不了 DLL")
        return False
    # 有几个头 `#include <vulkan/vulkan.h>`（`zlong/gpu/vulkan/*`），而构建树那套旗子里没有
    # Vulkan 的头目录 —— 钉到那些头的时候就会编不过。用**这台机器上那个 ASCII 路径**（包里的那份
    # 是 `运行环境/vulkan/…`，中文路径传不进命令行）。
    machine_vulkan = cache_value("Vulkan_INCLUDE_DIR")
    if machine_vulkan and Path(machine_vulkan).is_dir():
        flags.append(f"-I{Path(machine_vulkan).as_posix()}")

    # 一、每个有 `inline` 的头编一遍 —— 在自己那份文本上挂 `used`，把它们全钉出来。
    private = staging / "头"
    private.mkdir(parents=True, exist_ok=True)
    prelude = staging / "引子.h"
    prelude.write_text("".join(f"#include <{name}>\n" for name in BACKEND_PRELUDE),
                       encoding="utf-8")
    objects: list[Path] = []
    hooked = 0
    for index, header in enumerate(our_headers()):
        pinned, count = pin_functions(header.read_text(encoding="utf-8"))
        if count == 0:
            continue
        unit = private / f"{index:03d}.cpp"
        unit.write_text(pinned, encoding="utf-8")
        target = private / f"{index:03d}.o"
        done = subprocess.run([str(zig), "c++", *flags, "-include", prelude.as_posix(),
                               "-c", "-o", target.as_posix(), unit.as_posix()],
                              env=environment, capture_output=True, encoding="mbcs",
                              errors="replace", timeout=600)
        if done.returncode != 0:
            report.append(f"    后端：{header.relative_to(ROOT).as_posix()} 编不过")
            report.extend(cmake_verdict(done))
            return False
        objects.append(target)
        hooked += count
    if not objects:
        report.append("    后端：一个带 inline 的头都没有，这不正常")
        return False

    api = staging / "后端头.a"
    subprocess.run([str(zig), "ar", "rcs", api.as_posix(), *[o.as_posix() for o in objects]],
                   env=environment, capture_output=True, timeout=600)

    # 二、导出表。**只导我们自己的命名空间** —— libc++ 和 UCRT 的符号一个都不能出去。
    owned = [p for p in libraries if p.name.startswith("libzlong_")]
    folded = [p for p in libraries
              if any(word.lower() in p.name.lower() for word in FOLDED_IN)]
    symbols: set[str] = set(archive_symbols(api))
    for library in owned + folded:
        symbols |= set(archive_symbols(library))
    exported = sorted(name for name in symbols
                      if not name.startswith(".refptr.")
                      and any(ns in name for ns in EXPORTED_NAMESPACES))
    if not exported:
        report.append("    后端：导出表是空的，链出来也没人能调")
        return False
    exports = staging / f"{BACKEND}.def"
    exports.write_text("EXPORTS\n" + "".join(f"    {name}\n" for name in exported),
                       encoding="ascii")

    # 三、链。
    dll = staging / BACKEND_DLL
    importlib = staging / BACKEND_IMPORT
    command = [str(zig), "c++", "-std=c++20", "-shared", "-o", dll.as_posix(),
               *[path.as_posix() for path in owned],
               *[path.as_posix() for path in folded],
               api.as_posix(), exports.as_posix()]
    vulkan = cache_value("Vulkan_LIBRARY")
    if vulkan:
        command.append(Path(vulkan).as_posix())
    for name in BACKEND_SYSTEM_LIBS:
        command.append(f"-l{name}")
    command.append(f"-Wl,--out-implib,{importlib.as_posix()}")
    done = subprocess.run(command, env=environment, capture_output=True, encoding="mbcs",
                          errors="replace", timeout=1800)
    if done.returncode != 0 or not dll.is_file():
        report.append("    后端：DLL 链不起来")
        report.extend(cmake_verdict(done))
        return False

    target = package / ZIGLIBS
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(dll, target / BACKEND_DLL)
    shutil.copy2(importlib, target / BACKEND_IMPORT)
    report.append(f"  {ZIGLIBS}/{BACKEND_DLL}  {size_of(target / BACKEND_DLL)}"
                  f"（{len(owned) + len(folded)} 个库收成一个，{len(exported)} 条导出；"
                  f"钉住了 {hooked} 个函数）")
    return True


def build_cxx(package: Path, report: list[str]) -> tuple[Path, dict] | None:
    """编一遍项目，把 C++ 收成一个 DLL，把编译器和垫片一起铺进包里。

    回来的是 `(编译器, 编译环境)` —— 后面验头要用同一套，见 `verify_headers`。
    """
    zig = ROOT / ZIG / "zig.exe"
    if not zig.is_file():
        report.append(f"  C++：没有 {ZIG}/zig.exe")
        report.append("    把 zig 解到那儿就有了（https://ziglang.org，MIT，可再分发）")
        return None

    build = ROOT / ZIG_BUILD
    tools = zig_wrappers(zig, build / "tools")
    environment = dict(os.environ)
    environment["ZL_ZIG"] = str(zig)
    environment["ZL_COMPAT"] = str(ROOT / COMPAT)

    def cmake(*arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(["cmake", *arguments], env=environment, cwd=str(ROOT),
                              capture_output=True, encoding="mbcs", errors="replace",
                              timeout=3600)

    # Ninja 而不是 Visual Studio 生成器：整件事的意义就是不需要 Visual Studio。
    configured = cmake("-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                       f"-DCMAKE_C_COMPILER={tools['cc']}",
                       f"-DCMAKE_CXX_COMPILER={tools['cxx']}",
                       f"-DCMAKE_AR={tools['ar']}",
                       f"-DCMAKE_RANLIB={tools['ranlib']}",
                       "-DCMAKE_BUILD_TYPE=Release",
                       # 私有头要用它自己编引擎时的同一套旗子 —— 见 `compile_flags`。
                       "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                       # 测试一律不建，见 `TEST_SWITCHES`。
                       *(f"-D{name}=OFF" for name in TEST_SWITCHES),
                       # 机器在 Python 那一面也用它编：一个编译器，一套 ABI。`Python3_EXECUTABLE`
                       # 钉死成"正在跑打包的这个解释器" —— 包里的 Python 运行环境就是从它身上拷
                       # 的，编出来的模块的 ABI 标签（cp313）必须和它对得上。
                       "-DZL_BUILD_PYTHON_BINDINGS=ON",
                       f"-DPython3_EXECUTABLE={sys.executable}")
    if configured.returncode != 0:
        report.append("  C++：配置失败")
        report.extend(cmake_verdict(configured))
        return None

    built = cmake("--build", str(build))
    if built.returncode != 0:
        report.append("  C++：构建失败")
        report.extend(cmake_verdict(built))
        return None

    # **编译器和它编出来的库是一个整体**：库里是 GNU ABI 的目标文件，只有同一套工具链链得上。
    # 所以编译器进包 —— 它是**用代码的那个人的工具**，不是我们的东西；它自带的那一万多个
    # `.c/.cpp/.h` 是 Zig 自己的（libc / compiler-rt / libc++），不是这个项目的代码。
    copy_tree(zig.parent, package / ZIG, report)
    # 垫片：CMake 编这个项目时强制带的那个头，用户编自己的程序时也要带同一个。
    copy_tree(ROOT / COMPAT, package / COMPAT, report, what=f"  {COMPAT}/  编译垫片")

    # Vulkan：开发机上 CMake 找到的那份，连头带导入库一起放进来。项目里的头链着它，用户写的程序
    # 也一样 —— 少了它，一个 `#include <vulkan/vulkan.h>` 就编不过。
    vulkan_include = cache_value("Vulkan_INCLUDE_DIR")
    vulkan_library = cache_value("Vulkan_LIBRARY")
    if vulkan_include and Path(vulkan_include).is_dir():
        copy_tree(Path(vulkan_include), package / VULKAN / "include", report)
        report.append(f"  Vulkan 头 {size_of(package / VULKAN / 'include')}")
    if vulkan_library and Path(vulkan_library).is_file():
        target = package / VULKAN / "lib"
        target.mkdir(parents=True, exist_ok=True)
        shutil.copy2(vulkan_library, target / Path(vulkan_library).name)

    # 象棋和故宫的库不进包，见 `NOT_SHIPPED`。
    libraries = [p for p in build.rglob("*.a")
                 if "CMakeFiles" not in p.parts
                 and not p.name.endswith(".dll.a")
                 and not any(word in p.name.lower() for word in NOT_SHIPPED)]
    collected = build_dll(package, report, zig, environment, build, libraries)
    if not collected:
        report.append("    （C++ 没成，这一份里没有后端）")

    # **建模器跟着包走。** 它是个工具，不是"用内核写的程序"的样例：编辑器里「3D 建模」那一页起
    # 的就是它。包里没有源码、也没有谁去编它 —— 所以发的是成品。
    modeler = build / "宿主" / (MODELER_TARGET + EXE_SUFFIX)
    if modeler.is_file():
        (package / MODELER_DIR).mkdir(parents=True, exist_ok=True)
        target = package / MODELER_DIR / (MODELER + EXE_SUFFIX)
        shutil.copy2(modeler, target)
        report.append(f"  {MODELER_DIR}/{MODELER}{EXE_SUFFIX}  {size_of(target)}"
                      f" —— 编辑器的「3D 建模」那一页起的就是这一个")
    else:
        report.append(f"    缺：{MODELER_DIR}/{MODELER}{EXE_SUFFIX} —— C++ 那一步没编出来")
    return zig, environment


# --- 四、剩下的，和那条硬规矩 ------------------------------------------------------------------


def copy_rest(package: Path, report: list[str]) -> None:
    """手册，和子进程那个宿主程序。

    宿主程序发的是**字节码**（`live.pyc` / `frames.pyc`）—— 源码树里它是 `.py`，包里不是。
    （编译器、垫片、头、Vulkan 分别在 `build_cxx` 和 `copy_headers` 里铺。）
    """
    # 文档里那两个生成参考手册的 `.py` 是工具，是代码，不进产品。
    copy_tree(ROOT / "文档", package / "文档", report, what="  文档/",
              ignore_extra=("*.py", "__pycache__"))

    harness = package / HARNESS_DIR
    harness.mkdir(parents=True, exist_ok=True)
    for name in HARNESS:
        source = EDITOR / name
        if source.is_file():
            py_compile.compile(str(source), cfile=str(harness / (Path(name).stem + ".pyc")),
                               doraise=True)
    report.append(f"  {HARNESS_DIR}/  子进程的宿主程序（live.pyc + frames.pyc，字节码）")


def leftover_bodies(text: str) -> list[str]:
    """切完之后还带函数体的行。**这是另一双眼睛。**

    切和查要是同一套逻辑，切漏了查也漏 —— 上一版就是拿 `strip_bodies` 查 `strip_bodies`，结果
    `ram/registers.h` 那 15 个函数体一路漏到了成品里。所以这里故意写得笨：一行里有 `{`、`{` 前
    面有 `)`、而且这行不是 `if` / `for` / `while` 那种控制语句 —— 就算数。
    """
    templates = [(start, end) for _s, start, end, kind, _d in _functions(text)
                 if kind == "template"]
    found: list[str] = []
    offset = 0
    for line in text.splitlines(keepends=True):
        begin, offset = offset, offset + len(line)
        stripped = line.strip()
        if "{" not in stripped or ")" not in stripped.split("{")[0]:
            continue
        if stripped.startswith(("if", "for", "while", "switch", "catch", "else", "do", "case")):
            continue
        if any(begin < end and offset > start for start, end in templates):
            continue                      # 模板的体：C++ 规定它就得在头里
        found.append(stripped)
    return found


def copy_headers(package: Path, report: list[str]) -> None:
    """把接口铺进 `include/`。

    **我们自己的那些头要过一遍刀**（`strip_bodies`）：函数体挖掉、成员初始化列表挖掉、带花括号的
    常量初值换成 `extern` 声明。发出去的只剩类型、声明和常量 —— 正文在 `ziglib/zlong.dll` 里。
    第三方的（boost / dynarmic / vulkan）原样搬：它们本来就是源码，也不是这个项目要护的东西。

    **切完再切一遍，第二遍必须一处都切不出来。** 那是这条规矩的验收 —— 第一遍漏掉的那一类（实测
    漏过"写在类里的成员函数"），第二遍一定会切出来。它不依赖"我记得看过"。
    """
    target = package / INCLUDE
    target.mkdir(parents=True, exist_ok=True)
    ours = set()
    for pattern in OUR_HEADER_ROOTS:
        ours |= {root.resolve() for root in ROOT.glob(pattern)}
    copied = 0
    stripped = 0
    cuts = 0
    templates = 0
    slipped: list[str] = []
    for pattern in HEADER_ROOTS:
        for root in sorted(ROOT.glob(pattern)):
            if not root.is_dir():
                continue
            mine = root.resolve() in ours
            for file in root.rglob("*"):
                if not file.is_file() or file.suffix.lower() in NOT_A_HEADER:
                    continue
                destination = target / file.relative_to(root)
                destination.parent.mkdir(parents=True, exist_ok=True)
                if mine and file.suffix.lower() == ".h":
                    public, count = strip_bodies(file.read_text(encoding="utf-8"))
                    left = leftover_bodies(public)
                    if left:
                        slipped.append(f"{file.relative_to(ROOT).as_posix()}（{left[0][:48]}）")
                    destination.write_text(public, encoding="utf-8")
                    stripped += 1
                    cuts += count
                    templates += sum(1 for f in _functions(file.read_text(encoding="utf-8"))
                                     if f[3] == "template")
                else:
                    shutil.copy2(file, destination)
                copied += 1
    for name in HEADER_FILES:
        source = ROOT / name
        if source.is_file():
            public, count = strip_bodies(source.read_text(encoding="utf-8"))
            left = leftover_bodies(public)
            if left:
                slipped.append(f"{name}（{left[0][:48]}）")
            (target / source.name).write_text(public, encoding="utf-8")
            copied += 1
            stripped += 1
            cuts += count
    report.append(f"  {INCLUDE}/  {copied} 个头、{size_of(target)} —— 接口"
                  f"（自家 {stripped} 个切掉了 {cuts} 处实现；第三方那些原样）")
    if templates:
        report.append(f"    {templates} 处模板函数的体留着 —— C++ 规定它必须在头里"
                      f"（实例化的时候要看得到定义），这是唯一绕不开的一类")
    if slipped:
        report.append(f"    ！这些头切完还留着函数体：{'；'.join(slipped[:4])}")


def verify_headers(package: Path, report: list[str], zig: Path, environment: dict,
                   build: Path) -> None:
    """切完的头编不编得过 —— **编一遍，不是看着像对**。切的是文本，看着对没有任何意义。

    要看见包里那份（不是源码树里那份），所以 `-I` 给的是包里的 `include/`。
    """
    work = ROOT / BACKEND_BUILD / "验头"
    clear(work)
    work.mkdir(parents=True, exist_ok=True)
    base = compile_flags(build)
    if base is None:
        report.append("    验头：拿不到编译旗子，这一条没验")
        return
    # **旗子直接用构建树那一套，只把包里的 `include/` 插到最前面。** 自己拼踩过：`-include` 里那种
    # 中文路径根本传不进命令行（clang 收到的是按 ANSI 代码页重读过的字节）。
    flags = [f"-I{(package / INCLUDE).as_posix()}", *base]
    machine_vulkan = cache_value("Vulkan_INCLUDE_DIR")
    if machine_vulkan and Path(machine_vulkan).is_dir():
        flags.append(f"-I{Path(machine_vulkan).as_posix()}")

    bad: list[str] = []
    checked = 0
    for index, header in enumerate(our_headers()):
        text = header.read_text(encoding="utf-8")
        if strip_bodies(text)[1] == 0:
            continue                      # 这个头里没有实现，编不编都一样
        public, _ = strip_bodies(text)
        unit = work / f"{index:03d}.cpp"
        unit.write_text(public, encoding="utf-8")
        done = subprocess.run([str(zig), "c++", *flags, "-c", "-o",
                               unit.with_suffix(".o").as_posix(), unit.as_posix()],
                              env=environment, capture_output=True, encoding="mbcs",
                              errors="replace", timeout=600)
        checked += 1
        if done.returncode != 0:
            bad.append(header.relative_to(ROOT).as_posix())
            if len(bad) == 1:
                report.extend(cmake_verdict(done))
    if bad:
        report.append(f"    ！切完的头有 {len(bad)} 个编不过：{'、'.join(bad[:4])}")
    else:
        report.append(f"    切过的头编了一遍：{checked} 个全过")


def write_manifest(package: Path, report: list[str]) -> None:
    """把"编辑器编 C++ 要知道的一切"写成一个文件。

    编译宏、库表、头目录、Vulkan 都来自 `cpp.Project.discover` —— 打包脚本不自己解析 `.vcxproj`，
    它问编辑器要，用的是编辑器将来读清单的同一个格式。包里不再有 `build/`，所以要在打包时把开发机
    构建树里的答案转述一遍；转述之后每条路径都是**相对包根**的，整包挪到哪台机器上都对。
    """
    cpp = editor_package()
    project = cpp.Project.discover(ROOT)
    if project is None:
        report.append(f"  缺：{MANIFEST} 写不出来 —— 这台机器上的 build/ 还没构建过")
        return

    project.includes = [str(ROOT / INCLUDE)]
    if cache_value("Vulkan_INCLUDE_DIR"):
        # `运行环境/vulkan/` 只存在于包里，源码树里没有 —— 清单里的路径是相对项目根的，所以这里给
        # 的就是它在包里的位置。判断依据因此是"这台机器的 CMake 找到 Vulkan 了吗"。
        project.includes.append(str(ROOT / VULKAN / "include"))
    project.targets = []
    # **一个 DLL 顶掉一长串库。** 内核那八个、引擎、宿主音频，还有静态收进去的 dynarmic 那一套，
    # 现在都在 `zlong.dll` 里，用户那一行就是 `-lzlong`。系统库留着 —— 它们不在 DLL 里。
    project.libraries = [BACKEND] + [
        name for name in project.libraries
        if not name.startswith("zlong_")
        and not any(word.lower() in name.lower() for word in FOLDED_IN)
        and not any(word in name.lower() for word in NOT_SHIPPED)]
    if project.vulkan is not None:
        project.vulkan = ROOT / VULKAN / "lib" / Path(project.vulkan).name

    data = project.manifest(ROOT)
    (package / MANIFEST).write_text(json.dumps(data, ensure_ascii=False, indent=1),
                                    encoding="utf-8")
    report.append(f"  {MANIFEST}  {len(data['包含'])} 个头文件目录、{len(data['定义'])} 个宏、"
                  f"{len(data['库'])} 个库")


# --- 装 ----------------------------------------------------------------------------------------


def build(package: Path, with_exe: bool = True, with_python: bool = True,
          with_cxx: bool = True) -> list[str]:
    clear(package)
    package.mkdir(parents=True)
    report: list[str] = []

    # 图标先生成：exe 要嵌它，C++ 那一步（CMake 里的资源）也要用它。
    build_icons(report)

    # exe 先出。铺别的东西都慢，先铺的话整个打包过程的前一大半时间里这个包里看不出它最后会有一个
    # exe。
    if with_exe:
        build_exe(package, report)
    else:
        report.append("  exe：没带（--no-exe）")

    # 最慢的一步紧跟着，而且必须在 Python 之前：**机器的 Python 那一面（zlong 模块）也是这一步
    # 编出来的** —— 下面铺 Python 运行环境时要拿它。
    if with_cxx:
        compiler = build_cxx(package, report)
    else:
        report.append(f"  {RUNTIME}/：C++ 那一套没带（--no-cxx）")
        compiler = None

    if with_python:
        build_python(package, report)
    else:
        report.append(f"  {RUNTIME}/python/：没带（--no-python）")

    # 接口铺进去 —— 我们自己的头在这里被切掉实现，正文已经在 DLL 里了。
    copy_headers(package, report)
    # 铺完就**拿包里那份头编一遍**：切的是文本，不编一遍等于没验。
    if compiler is not None:
        verify_headers(package, report, *compiler, ROOT / ZIG_BUILD)
    compile_python(package, report)
    copy_rest(package, report)
    write_manifest(package, report)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="把烛龙装成一个可以拷走的目录")
    parser.add_argument("--into", type=Path, default=None, help="装到哪儿（默认 发行/）")
    parser.add_argument("--name", default=None, help="目录名（默认 烛龙-<日期>）")
    parser.add_argument("--zip", action="store_true", help="装好之后压成 zip")
    parser.add_argument("--no-exe", action="store_true", help="不冻 exe")
    parser.add_argument("--no-python", action="store_true", help="不带 Python 运行环境")
    parser.add_argument("--no-cxx", action="store_true", help="不带 C++ 编译器（跳掉最慢的一步）")
    arguments = parser.parse_args()

    name = arguments.name or f"烛龙-{date.today().isoformat()}"
    package = (arguments.into or (ROOT / "发行")) / name

    print(f"装到 {package}\n")
    started = time.monotonic()
    report = build(package, with_exe=not arguments.no_exe,
                   with_python=not arguments.no_python, with_cxx=not arguments.no_cxx)
    print("\n".join(report))

    missing = [name for name in (f"{EXE}{EXE_SUFFIX}",) if not (package / name).is_file()]
    if missing:
        print(f"\n[包里没有 {'、'.join(missing)} —— 这一份不完整]")

    print(f"\n大小 {size_of(package)}，用时 {time.monotonic() - started:.0f} 秒")

    if arguments.zip:
        archive = package.with_suffix(".zip")
        print(f"\n压缩到 {archive}")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
            for path in sorted(package.rglob("*")):
                if path.is_file():
                    bundle.write(path, path.relative_to(package.parent))
        print(f"压缩后 {archive.stat().st_size / 1e6:.0f} MB")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
