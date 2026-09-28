"""把每个头文件里的公开声明抽成 markdown，好让参考文档不会跟代码走散。

    python 文档/提取签名.py                 写到 文档/参考/
    python 文档/提取签名.py --print 内核/RAM  打到屏幕上看看

这不是个 C++ 解析器，是个**摘录**工具：它把每个头文件开头的说明、以及每个带注释的声明原样
搬过来。所以它给不了语义，但给得了**准确** —— 签名是从源码里抄的，不可能抄错，而手写的部分
（怎么用、为什么这么设计、哪儿有坑）本来就不该由机器来编。

用法上，它和手写的文档是一对：手写的说"这一层是干什么的、怎么用"，这里说"它到底有哪些东西"。
头文件改了，重跑一次就对上了。

（Python 那一面有它自己的一份，见 `提取绑定.py`：那一份不是抄头文件，是 `import zlong` 之后自省。
两者覆盖的不是同一批东西 —— 绑出去的东西比头文件少得多。）
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = Path(__file__).resolve().parent / "参考"

# 每一层：显示名 -> 头文件目录。
AREAS = {
    "内核-ram": "内核/RAM/include",
    "内核-ssd": "内核/SSD/include",
    "内核-cpu": "内核/cpu/include",
    "内核-gpu": "内核/gpu/include",
    "内核-audio": "内核/audio/include",
    "内核-mount": "内核/mount/include",
    "内核-service": "内核/service/include",
    "内核-system": "内核/system/include",
    "引擎": "引擎/include",
    "宿主": "宿主",
}

# 一个声明长什么样：带括号的多行签名、类/结构/枚举、typedef、常量。抽出来的是原样的行。
_DECLARATION = re.compile(
    r"^\s*(?:"
    r"(?:class|struct|enum(?:\s+class)?|union)\s+\w+"
    r"|(?:using|typedef)\s+\w+"
    r"|(?:inline\s+)?(?:constexpr|static|virtual|explicit|friend|template)\b.*"
    r"|\w[\w:<>,\s\*&\[\]\.]*\b\w+\s*\("
    r")"
)

# 函数体里的语句也长得像声明，别把它们当成接口抄进来。
_STATEMENT = re.compile(
    r"^\s*(?:if|for|while|return|switch|else|do|break|continue|case|goto|throw|assert|"
    r"static_assert|auto\s+\w+\s*=|std::)\b"
)


def doc_comment_above(lines: list[str], index: int) -> list[str]:
    """The `///` block immediately above a declaration, which is how this project explains itself."""
    out: list[str] = []
    at = index - 1
    while at >= 0:
        stripped = lines[at].strip()
        if stripped.startswith("///"):
            out.append(stripped[3:].strip())
            at -= 1
        elif stripped.startswith("//") and not stripped.startswith("///"):
            out.append(stripped[2:].strip())
            at -= 1
        elif not stripped:
            # A blank line ends the block, unless the line above it was a comment.
            if out:
                break
            at -= 1
        else:
            break
    out.reverse()
    return out


def extract(path: Path) -> str:
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    # 文件开头那段说明：第一个非空之后连续的注释块。
    header: list[str] = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("///") or stripped.startswith("//"):
            header.append(stripped.lstrip("/").strip())
        elif stripped.startswith("#") or not stripped:
            continue
        else:
            break

    body: list[str] = []
    brace_depth = 0
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped:
            continue

        # 类/结构/枚举/函数：记下注释和这一行（多行签名往后接）。缩进留着 —— 看得出谁在谁里面。
        if (_DECLARATION.match(line) and not _STATEMENT.match(line) and brace_depth <= 2):
            comment = doc_comment_above(lines, index)
            indent = line[: len(line) - len(line.lstrip())]
            if comment:
                for part in comment:
                    body.append(f"{indent}// {part}" if part else f"{indent}//")
            body.append(line.rstrip())
            # 签名可能换行：一直接到出现 { 或 ;
            lookalike = index
            while not any(ch in lines[lookalike] for ch in "{;") and lookalike + 1 < len(lines):
                lookalike += 1
                body.append(lines[lookalike].rstrip())

        brace_depth += line.count("{") - line.count("}")

    parts = [f"# `{path.name}`", ""]
    parts.append(f"`{path.relative_to(ROOT).as_posix()}`")
    parts.append("")
    if header:
        parts.append("```")
        parts.extend(header)
        parts.append("```")
        parts.append("")
    if body:
        parts.append("```cpp")
        parts.extend(body)
        parts.append("```")
    return "\n".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description="把头文件里的声明抽成 markdown")
    parser.add_argument("--print", dest="show", default=None, help="只打某一层，不写文件")
    arguments = parser.parse_args()

    if arguments.show:
        folder = ROOT / AREAS[arguments.show]
        for path in sorted(folder.rglob("*.h")):
            print(extract(path))
            print()
        return 0

    OUTPUT.mkdir(parents=True, exist_ok=True)
    index = ["# 参考 — 声明，从代码里抄的", "",
             "`参考/` 里只有**声明**：C++ 那一面是 `文档/提取签名.py` 从头文件里抄的，Python 那一面是",
             "`文档/提取绑定.py` 从编出来的模块自省出来的。所以两边都不会跟代码走散。",
             "", "怎么用、为什么这么做、哪个该调哪个不该调，在 [`08-从包里调组件.md`](../08-从包里调组件.md)",
             "和别的手写文档里。", "",
             "- [Python-绑定](Python-绑定.md) — `import zlong` 拿到的全部"
             "（**由 `提取绑定.py` 生成，不是这一份**）"]
    for area, relative in AREAS.items():
        folder = ROOT / relative
        headers = sorted(folder.rglob("*.h"))
        if not headers:
            continue
        pieces = [f"# {area}", ""]
        for path in headers:
            pieces.append(extract(path))
            pieces.append("")
            pieces.append("---")
            pieces.append("")
        (OUTPUT / f"{area}.md").write_text("\n".join(pieces), encoding="utf-8")
        index.append(f"- [{area}]({area}.md) — {len(headers)} 个头文件")
        print(f"{area}: {len(headers)} 个头文件 -> 文档/参考/{area}.md")
    (OUTPUT / "README.md").write_text("\n".join(index) + "\n", encoding="utf-8")
    print("文档/参考/README.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
