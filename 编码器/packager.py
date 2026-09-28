"""把一个游戏打成一个能拿走的包。

**一个游戏是一个文件夹** —— 和编辑器里"一个文件夹就是一个程序"是同一条规矩。按「打包」时，
游戏就是这个文件所在的那一层。

**游戏是 C++ 写的。** 引擎是 C++，所以游戏打包只有一条路：把这个文件夹里的源文件编成一个 exe。
（这里曾经还认过 Python —— 按"你在看哪个文件"猜语言，结果把 `编码器/` 自己当成一个十三个脚本的
Python 游戏去冻。这条已经去掉：工具不该猜，而且猜错的那一次代价很荒唐。）

产物长这样：

    <游戏名>/
        <游戏名>.exe        游戏的代码，编成一个 exe
        依赖库/             它自己之外、跑起来要有的东西
        <数据文件>          游戏文件夹里不是源码的东西，原样放在 exe 旁边

**为什么数据放在 exe 旁边，而不是塞进 `依赖库/`**：程序里写的是 `LoadScene("关卡.scene")`
这样的相对路径。把一个文件挪到子目录里，那些路径就全断了。

**为什么 `依赖库/` 通常是空的**：内核是以静态库链进去的，C++ 运行时（libc++、libunwind）也是
静态链的。实测一个 Zig 编出来的 exe，导入表里只有 `KERNEL32.dll` 和 `api-ms-win-crt-*`
（后者是 Windows 的 API set，不是磁盘上的文件）—— 也就是说它就是自足的。这是好事，不是没做完。
"""

from __future__ import annotations

import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

import cpp

# 放数据文件时**不碰**的东西：这些是工作室自己的零件，不是游戏的一部分。
# 没有这一条，一个放在包根上的游戏会把 400 MB 的运行环境当成"数据"拷一遍。
_NOT_DATA = {
    "运行环境", "依赖库", "include", "工具链", "文档", "发行", "build", "build-zig",
    "build-打包", "__pycache__", ".vscode",
}
_NOT_DATA_FILES = {"项目清单.json", "CMakeLists.txt", "打包.py", "烛龙.exe"}

# 什么算"游戏的代码"。
CXX_SOURCES = (".cpp", ".cc", ".cxx", ".c++")
# 头文件是代码的一部分，不是"数据" —— 它们要跟着源文件走，不该拷到 exe 旁边去。
HEADERS = (".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp")
# 脚本也一样：它是代码，不是数据。把它拷到 exe 旁边会让人以为程序会读它。
SCRIPTS = (".py", ".pyw")

# 工作室自己的零件，不是游戏。
#
# **这条护栏是踩出来的**：在 `编码器/` 里按了「打包游戏」，它把这个编辑器本身当成了一个"13 个
# 脚本的 Python 游戏"。`编码器/`、`内核/`、`引擎/`、`宿主/` 这些是**做游戏用的东西**，不是游戏。
_NOT_A_GAME = {"编码器", "内核", "引擎", "宿主", "运行环境", "依赖库", "include", "工具链",
               "文档", "external", "发行", "build", "build-zig"}


@dataclass
class Plan:
    """要打的东西，以及打到哪。"""

    folder: Path                     # 游戏文件夹（"这个游戏"的全部）
    name: str                        # 游戏名，也是 exe 的名字
    sources: list[Path] = field(default_factory=list)
    data: list[Path] = field(default_factory=list)
    output: Path = Path(".")         # 产物落在哪（<...>/<游戏名>/）
    leftovers: list[str] = field(default_factory=list)   # 清掉的编译中间物，报账用

    @property
    def program(self) -> Path:
        return self.output / (self.name + ".exe")

    @property
    def dependencies(self) -> Path:
        return self.output / "依赖库"


def plan_for(source: Path, name: str | None = None,
             project: "cpp.Project | None" = None) -> Plan:
    """按"这个文件属于哪个游戏"算出一份打包计划。

    **源文件要用编辑器自己那套说法**（`Project.sources_for`），不要在这儿另写一条。写重了会出事：
    `宿主/xiangqi/` 里有六个 `.cpp`，但项目自己的说法是这个程序 = 五个源文件 + `zlong_xiangqi_rules`
    库（`rules.cpp` 属于那个库）。要是照着文件夹扫六个，`rules.cpp` 就会和库里的定义撞成一片
    `duplicate symbol`。

    `project` 为 None（比如没有项目清单）时退化成"这个文件夹里所有 `.cpp`"。
    """
    folder = source.resolve().parent
    if project is not None and project.sources_for(source) != [source]:
        sources = [p for p in project.sources_for(source) if p.suffix.lower() in CXX_SOURCES]
    else:
        sources = sorted(p for p in folder.iterdir()
                         if p.is_file() and p.suffix.lower() in CXX_SOURCES)

    return Plan(folder=folder,
                name=name or folder.name,
                sources=sources,
                data=data_in(folder))


def refusal(plan: Plan) -> str:
    """这个计划能不能执行。不能的话说清为什么。

    **两条护栏，都是踩出来的。**

    一、**产物不能落在游戏文件夹里。** 打包第一步是清空输出目录，落在里面就等于把游戏自己删了。
    默认的 `<游戏文件夹的上一级>/发行/<名字>` 不会撞，但用户改名字就可能，所以拦在这里。

    二、**不能拿工作室自己的零件当游戏。** 引擎是 C++，`编码器/`、`内核/`、`引擎/`、`宿主/`
    这些是**做游戏用的东西**，不是游戏 —— 把它们打成包没有意义。
    """
    if plan.folder.name in _NOT_A_GAME:
        return (f"{plan.folder}\n是工作室自己的零件，不是游戏。\n"
                f"这些是**做游戏用的东西** —— 引擎、工具、内核 —— 拿来打包没有意义。\n"
                f"一个游戏是它自己那个文件夹。")
    output = plan.output.resolve()
    folder = plan.folder.resolve()
    if output == folder or folder in output.parents:
        return (f"产物目录不能落在游戏文件夹里面：\n{output}\n"
                f"打包会先清空输出目录 —— 那等于把游戏自己删掉。")
    return ""


def data_in(folder: Path) -> list[Path]:
    """文件夹里不是源码、也不是工作室零件的东西 —— 关卡、图片、声音、说明。"""
    kept: list[Path] = []
    for path in sorted(folder.iterdir()):
        if path.is_dir():
            if path.name in _NOT_DATA or path.name.startswith("build"):
                continue
            kept.extend(sorted(p for p in path.rglob("*") if p.is_file()))
        elif path.is_file():
            if path.name in _NOT_DATA_FILES:
                continue
            if path.suffix.lower() in CXX_SOURCES + HEADERS + SCRIPTS:
                continue
            if path.suffix.lower() in (".obj", ".o", ".pdb", ".ilk", ".exe", ".pyc"):
                continue
            kept.append(path)
    return kept


def cxx_arguments(plan: Plan, project: cpp.Project, toolchain: cpp.Toolchain) -> list[str]:
    """编一个游戏的命令行。

    和按 F5 编一个散装程序是同一条路（同一个编译器、同一套头文件和库），只有两个不同：源文件是
    计划里那一串，产物落到输出目录、并且叫游戏的名字。

    **不造 `依赖库/`** —— 见文件开头：通常一样都不需要。
    """
    plan.output.mkdir(parents=True, exist_ok=True)
    arguments = ["c++", "-std=c++20", "-O2", "-w"]
    if toolchain.compat is not None:
        arguments += [f"-include{toolchain.compat / 'mingw_compat.h'}",
                      f"-I{toolchain.compat / 'include'}"]
    arguments += [str(path) for path in plan.sources]
    arguments += [f"-I{path}" for path in project.includes]
    arguments += [f"-D{name}" for name in project.definitions]
    arguments += ["-o", str(plan.program)]
    arguments += project.link_arguments(project.libraries)
    return arguments


# --- 收尾 -------------------------------------------------------------------------------------


def clear(folder: Path) -> None:
    """把旧的产物清掉，等一等还在读它的东西。

    和 `打包.py` 里那个是同一个道理：Windows 上刚跑完的东西可能被索引器或杀毒软件攥着一瞬间，
    于是"再打一次"变成权限错误。那是环境噪音，重试而不是报错。
    """
    if not folder.exists():
        return
    for attempt in range(12):
        try:
            shutil.rmtree(folder)
            return
        except OSError:
            if attempt == 11:
                raise
            time.sleep(0.5 + 0.25 * attempt)


def tidy(plan: Plan) -> list[str]:
    """把编译的中间产物从产物目录里清掉。

    **不是洁癖**：Zig 链 Windows 目标时一定会写一个 `.pdb`，实测一个 4.7 MB 的 exe 旁边躺着
    **58 MB** 的调试库 —— 而且 `-g0` 关不掉它（试过）。那是编译器的工作文件，不是游戏的一部分。

    只看产物目录这一层，不往里挖：数据文件是拷进来的，不动它们。
    """
    if not plan.output.is_dir():
        return []
    removed = []
    for path in sorted(plan.output.iterdir()):
        if path.is_file() and path.suffix.lower() in (".pdb", ".lib", ".obj", ".o", ".ilk",
                                                      ".exp", ".d"):
            removed.append(path.name)
            path.unlink()
    return removed


def copy_data(plan: Plan) -> int:
    """数据文件原样放到 exe 旁边 —— 相对路径因此不用改。"""
    copied = 0
    for path in plan.data:
        target = plan.output / path.relative_to(plan.folder)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        copied += 1
    return copied


def report(plan: Plan) -> list[str]:
    """打完之后的账。"""
    lines = [f"  游戏        {plan.name}"]
    if plan.program.is_file():
        lines.append(f"  {plan.name}.exe  {plan.program.stat().st_size / 1e6:.1f} MB")
    inside = ([p for p in plan.dependencies.rglob("*") if p.is_file()]
              if plan.dependencies.is_dir() else [])
    if inside:
        total = sum(p.stat().st_size for p in inside)
        lines.append(f"  依赖库/     {len(inside)} 个文件、{total / 1e6:.1f} MB")
    else:
        lines.append("  依赖库/     不需要 —— 内核和 C++ 运行库都是静态链进去的")
    if plan.data:
        lines.append(f"  数据        {len(plan.data)} 个文件，放在 exe 旁边")
    if plan.leftovers:
        lines.append(f"  清掉        {len(plan.leftovers)} 个编译中间物"
                     f"（{'、'.join(plan.leftovers)}）")
    lines.append(f"  在哪        {plan.output}")
    return lines
