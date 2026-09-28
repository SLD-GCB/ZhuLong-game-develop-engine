"""把 `import zlong` 拿到的整个模块表面写成一份 markdown，好让 Python 那一面的参考也不跟代码走散。

    python 文档/提取绑定.py                   写到 文档/参考/Python-绑定.md

和 `提取签名.py` 是一对，但**取材的地方不同**，这是关键：

* `提取签名.py` 读**头文件**，抄的是 C++ 的声明；
* 这一个读**编出来的模块**（`import zlong` 然后自省），抄的是 C++ 那一面真正绑出去的东西。

两者不是同一件事 —— `bindings.cpp` 只绑了整个机器的一小部分，而「少绑了什么」是光看头文件看不出来
的。所以这份页面里**没有** `Kernel`、没有 `Renderer`、没有 `scene_file`，那是实话：Python 拿不到
它们。至于为什么，以及该用哪个，在 `08-从包里调组件.md` 里。

**签名不是从源码里解析出来的，是问模块要的。** pybind11 给每个方法都留了带签名的 docstring，
所以这里只要 `__doc__`，不用猜也不用解 C++。代价是原始文本很机械（`typing.SupportsInt |
typing.SupportsIndex` 就是「任意整数」），所以有一层**改写**把它折成人看的名字 —— 改写只做三件事：
把两种 Supports 折成 `int` / `float`，把 `zlong.X` / `zlong::engine::X` 折成 `X`，把 `self: ...`
去掉。语义一个字没动。

**没装 zlong 就直说。** 这一份要么在源码树里构建过一次之后跑（模块在 `build/**`），要么用包里那个
解释器跑（模块在 `site-packages/`）。两条都够不着的时候它退出并说清楚，而不是产出一份空页面 ——
一份「什么也没有」的参考比没有参考更坏。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = Path(__file__).resolve().parent / "参考" / "Python-绑定.md"

# 机器在 Python 那一面可能落在哪儿。和 `编码器/runtime.py` 找的是同一批地方。
_BACKEND_GLOBS = (
    "运行环境/**/zlong*.pyd",
    "运行环境/**/zlong*.so",
    "build/**/zlong*.pyd",
    "build/**/zlong*.so",
)

# 页面怎么分段、每段里谁在前。**这是一张要维护的表，不是装饰**：模块里多绑一个名字而没写进这里，
# 下面会直接报错，那样才不会被静默漏掉 —— 和 `打包.py` 里那份 `EDITOR_MODULES` 是同一个用意。
SECTIONS = (
    ("数据 —— 一个场景由这些组成", (
        "Vec3", "Camera", "Light", "Texture", "Material", "Mesh", "Node", "Scene",
    )),
    ("常量 —— 不指向任何东西时的下标", (
        "kNoMesh", "kNoTexture", "kNoParent",
    )),
    ("程序化图元 —— 还没有内容管道，这就是全部的建模词汇", (
        "make_box", "make_grid", "make_cylinder", "make_cone", "make_disc", "make_sphere",
        "make_checker_texture", "make_ripple_normal_texture",
    )),
    ("变换 —— `Mat4` 对 Python 是不透明的，只能用这些造出来", (
        "Mat4", "identity", "translation", "rotation_y", "scale", "scale_xyz", "radians",
    )),
    ("机器 —— 那台机器，和它回调出来的一侧", (
        "Platform", "FrameResult", "System",
    )),
)

ORDER = tuple(name for _, names in SECTIONS for name in names)

# 值得列出来的运算符。`__mul__` 是唯一必要的那个 —— 摆放一件东西就是矩阵相乘。
_WORTH_SHOWING = ("__mul__",)


def load_module():
    """`zlong` 模块，从它可能在的任何一个地方。"""
    try:
        import zlong                                      # noqa: PLC0415 - 见下：路径要先铺好
        return zlong
    except ImportError:
        pass

    for pattern in _BACKEND_GLOBS:
        for hit in sorted(ROOT.glob(pattern)):
            folder = str(hit.parent)
            if folder not in sys.path:
                sys.path.insert(0, folder)
            try:
                import zlong                                  # noqa: PLC0415
                return zlong
            except ImportError:
                continue

    raise SystemExit(
        "import 不到 zlong —— 这一份是从**编出来的模块**抄的，所以先得有它。\n"
        "  源码树里：先跑一次 `python 打包.py`（或任何一次 Zig 构建），模块在 build-zig/ 或 build/ 下\n"
        "  只有包：用包里那个解释器跑本脚本，`运行环境/python/python.exe 文档/提取绑定.py`")


def readable(text: str) -> str:
    """把 pybind11 的机械类型名折成人看的那几个。语义不动，只换写法。"""
    for machine, human in (
            ("typing.SupportsFloat | typing.SupportsIndex", "float"),
            ("typing.SupportsInt | typing.SupportsIndex", "int"),
            ("collections.abc.Sequence", "Sequence"),
            ("zlong::engine::", ""),
            ("zlong.", ""),
    ):
        text = text.replace(machine, human)
    return text


def signature(attr, name: str) -> str:
    """一个方法的一行签名，从它自己的 docstring 里来。

    docstring 的第一行就是签名本身（pybind11 写的），后面可能还有一段说明，这里不要 ——
    说明在头文件里，`参考/` 的 C++ 那一半抄的就是它。
    """
    first = (attr.__doc__ or "").strip().splitlines()
    if not first:
        return f"{name}(…)"
    line = readable(first[0].strip())
    line = re.sub(rf"^{re.escape(name)}\(self\)", f"{name}()", line)
    line = re.sub(rf"^{re.escape(name)}\(self, ", f"{name}(", line)
    return line


def members_of(cls) -> tuple[list[tuple[str, str]], list[str]]:
    """一个类底下能被调用的东西：属性一张表，方法一行一个。

    按名字排序，而且是从 `dir()` 枚举出来的 —— 所以多一个方法不会漏，只会在下一次重跑时出现。
    嵌在里面的类型（`System.Config`）不算方法，它自己在下面另起一段。
    """
    properties: list[tuple[str, str]] = []
    methods: list[str] = []
    for name in sorted(dir(cls)):
        if name.startswith("_") and name not in _WORTH_SHOWING:
            continue
        attr = getattr(cls, name)
        if isinstance(attr, type):
            continue
        if isinstance(attr, property):
            properties.append((name, "可读写" if attr.fset is not None else "只读"))
        elif callable(attr):
            methods.append(signature(attr, name))
    return properties, methods


def class_page(name: str, cls) -> list[str]:
    """一个类：属性表、方法块，然后是嵌在它里面的类型。

    这一段**不带结尾的分隔线**（调用方加）：`System` 和它里面的 `System.Config` 要接成一段，
    各带一条就会连出两条线来。
    """
    lines = [f"### `{name}`", ""]

    if hasattr(cls, "__members__"):                      # 一个 enum：列成员
        lines += ["| 成员 | 值 |", "|---|---|"]
        for member in cls.__members__.values():
            lines.append(f"| `{member.name}` | `{member.value}` |")
        lines.append("")
        return lines

    properties, methods = members_of(cls)
    if properties:
        lines += ["| 属性 | |", "|---|---|"]
        lines += [f"| `{member}` | {note} |" for member, note in properties]
        lines.append("")
    if methods:
        lines += ["```python", *methods, "```", ""]

    for member in sorted(dir(cls)):
        nested = getattr(cls, member)
        if not member.startswith("_") and isinstance(nested, type):
            lines += ["---", ""]
            lines += class_page(f"{name}.{member}", nested)
    return lines


def main() -> int:
    module = load_module()

    public = {name for name in dir(module) if not name.startswith("_")}
    ordered = set(ORDER)
    missing = sorted(ordered - public)
    extra = sorted(public - ordered)
    if missing or extra:
        for name in missing:
            print(f"！SECTIONS 里的 {name} 模块里没有了 —— 从 文档/提取绑定.py 里删掉它")
        for name in extra:
            print(f"！模块里多了 {name}，SECTIONS 里没有 —— 加上它，并决定放在哪一段")
        raise SystemExit("分段表要和模块对上才写页面")

    parts = [
        "# Python 绑定 — `import zlong` 拿到的全部",
        "",
        "这一份是 `文档/提取绑定.py` 从**编出来的模块**自省出来的：签名是问 pybind11 要的，不是解出来的。",
        "所以它不可能跟 `内核/system/python/bindings.cpp` 走散 —— 那一面变了，重跑一次就对上了。",
        "",
        "> 机器只在 Python 这一面露出这么多。**内核、GPU、渲染器、场景文件都不在这儿** —— 那不是说",
        "> 它们不重要，是说这一层绑定有意做窄：前端拿到的是 `System`，不是它底下的机器。要那些，",
        "> 走 C++（见 [`参考/引擎.md`](引擎.md)、[`参考/内核-system.md`](内核-system.md)）。",
        "",
        f"模块自己的话：*{module.__doc__}*",
        "",
        "两个读法上的注：`arg0` 是绑定时**没有起名字**的参数（不是「第一个参数」的意思），",
        "`int` / `float` 是 pybind11 那句很长的 `SupportsInt | SupportsIndex` 折过来的。",
        "",
        "---",
        "",
    ]

    for title, names in SECTIONS:
        parts += [f"## {title}", ""]
        functions: list[str] = []
        constants: list[tuple[str, object]] = []
        for name in names:
            attr = getattr(module, name)
            if isinstance(attr, type):
                parts += class_page(name, attr)
                parts += ["---", ""]
            elif callable(attr):
                functions.append(signature(attr, name))
            else:
                constants.append((name, attr))
        if functions:
            parts += ["```python", *functions, "```", "", "---", ""]
        if constants:
            parts += ["```python", *(f"{n} = {v!r}" for n, v in constants), "```", "",
                      "（「空」的那个下标。和 C++ 那边同名的常量是一回事。）", "", "---", ""]

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(parts).rstrip() + "\n", encoding="utf-8")
    print(f"{len(ORDER)} 个名字 -> {OUTPUT.relative_to(ROOT).as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
