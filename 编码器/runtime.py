"""Running what the editor edits, against the machine this project builds.

Two kinds of child come out of a Run:

* a **script**, which is ordinary Python that can say `import zlong` and get the whole kernel +
  engine + host stack as a module;
* a **program**, which is a C++ file compiled and linked against the libraries in `build/`.

They differ in how they are prepared and in almost nothing else, so both are a `Command` -- a
program, its arguments, a working directory, an environment and how to read what it says -- and one
`Runner` watches whichever is going.
"""

from __future__ import annotations

import os
import shlex
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from PySide6.QtCore import QObject, QProcess, QProcessEnvironment, Signal

import frames

# Where the machine's Python module can end up, relative to the project root. A package ships it in
# the bundled interpreter's `site-packages`, so `import zlong` needs nothing set up at all; a source
# checkout has it wherever CMake put it under `build/`.
_MODULE_GLOBS = (
    "运行环境/**/zlong*.pyd",
    "运行环境/**/zlong*.so",
    "运行环境/**/zlong*.dylib",
    "build/**/zlong*.pyd",
    "build/**/zlong*.so",
    "build/**/zlong*.dylib",
)

# The interpreter and the compiler that ship with the software, each one thing and no substitutes.
RUNTIME = "运行环境"
PYTHON_IN_RUNTIME = "python"

# The child-process harness lives **inside** the runtime, not next to the product. It is a piece of
# the machinery -- the interpreter runs it, `frames.py` is the wire it speaks to the editor -- and
# a product folder should show a product, not the implements. It is a directory of its own so that
# `import frames` resolves to the one beside it and to nothing else.
HARNESS_DIR = "harness"


def frozen() -> bool:
    """True when this editor was packaged into an exe rather than run from source."""
    return bool(getattr(sys, "frozen", False))


def editor_asset(name: str) -> Path:
    """编辑器自己带着的一个小文件（图标那类）。

    成品里它在 `依赖库/` 底下 —— `打包.py` 用 PyInstaller 的 `--add-data` 放过去的，而那一格正好
    是「程序之外、跑起来要有的东西」。从源码跑的时候它就在 `编码器/` 里。
    """
    if frozen():
        return project_root() / "依赖库" / name
    return Path(__file__).resolve().parent / name


def project_root() -> Path:
    """The project this editor edits.

    Frozen, the editor is an exe sitting at the top of the package, so the package root is the folder
    it is in. Run from source, it is two levels up from this file (编码器/runtime.py).
    """
    if frozen():
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


def interpreter() -> str:
    """What runs a user's script.

    Always the interpreter that ships with the software -- `运行环境/python/python.exe`. Not
    `sys.executable` blindly: frozen, that is the editor's own exe, and an exe built by PyInstaller
    is not an interpreter -- handing it `-u live.py 脚本.py` does nothing useful. Run from a source
    checkout, where nothing ships, the interpreter running the editor is the only one there is.
    """
    bundled = project_root() / RUNTIME / PYTHON_IN_RUNTIME / "python.exe"
    return str(bundled) if bundled.is_file() else sys.executable


def find_backend() -> Path | None:
    """Directory holding the built `zlong` module, or None if the project has not been built."""
    root = project_root()
    for pattern in _MODULE_GLOBS:
        for hit in sorted(root.glob(pattern)):
            return hit.parent
    return None


def script_environment() -> dict[str, str]:
    """The child's environment: a clean one, plus the import roots a script needs.

    **What the machine has set about Python is dropped, not inherited.** The software is meant to
    run its code in its own environment: a `PYTHONPATH` or `PYTHONHOME` left over from somebody's
    other installation is exactly the kind of thing that makes `import zlong` work on one machine
    and fail on the next, for reasons that are invisible from inside the editor.

    The backend's folder goes on `PYTHONPATH` so `import zlong` works; the project root goes on too,
    so a script can reach the host programs' own modules. The script's own folder does not need
    adding -- Python puts it there already.
    """
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith("PYTHON")}

    roots: list[str] = []
    backend = find_backend()
    if backend is not None:
        roots.append(str(backend))
    roots.append(str(project_root()))
    env["PYTHONPATH"] = os.pathsep.join(roots)

    # Keep the child's output unbuffered so the console fills as it runs, not at the end.
    env["PYTHONUNBUFFERED"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    return env


def split_arguments(text: str) -> list[str]:
    """Split a command line into arguments.

    Not `shlex.split`, because a backslash is an escape there: `D:\\project\\out.png` comes back as
    `D:projectout.png` and the script writes a file nobody asked for. On Windows a backslash is a
    path separator, so the splitter here is the one the shell actually uses — quotes group, spaces
    separate, backslashes are just characters. Elsewhere `shlex` is right.
    """
    if os.name != "nt":
        return shlex.split(text)

    arguments: list[str] = []
    current: list[str] = []
    started = False
    quote: str | None = None

    for char in text:
        if quote is not None:
            if char == quote:
                quote = None
            else:
                current.append(char)
        elif char in "\"'":
            quote = char
            started = True
        elif char.isspace():
            if started:
                arguments.append("".join(current))
                current = []
                started = False
        else:
            current.append(char)
            started = True

    if quote is not None:
        raise ValueError("没有闭合的引号")
    if started:
        arguments.append("".join(current))
    return arguments


@dataclass
class Command:
    """Something to run, and how to watch it."""

    program: str
    arguments: list[str] = field(default_factory=list)
    working_directory: Path = Path(".")
    environment: dict[str, str] | None = None   # None means "inherit"
    encoding: str = "utf-8"
    # True when the child sends pictures down stderr and words down stdout. False merges the two, so
    # that everything a compiler says lands in the console in the order it said it.
    frames: bool = False


def harness() -> Path:
    """The script the editor runs *instead of* a Python file, so that it has a host.

    It lives beside this file and is run as a file, which puts this directory first on the child's
    import path; it moves that aside for the script's own directory as soon as it starts.

    Frozen, this file is not inside the exe -- a child interpreter needs a file to read. The packager
    puts it in the runtime (`运行环境/python/harness/`), which is where the machinery belongs;
    `frames.py` sits beside it, so `import frames` finds that one and nothing else.

    **成品里它是字节码。** 包里一个 `.py` 都不许有（见 `打包.py` 的 `SOURCE_SUFFIXES`），所以打出来的
    是 `live.pyc`；CPython 从一个没有 `.py` 的 `.pyc` 照样跑。源码树里还是 `.py`。
    """
    if frozen():
        runtime = project_root() / RUNTIME / PYTHON_IN_RUNTIME / HARNESS_DIR
        compiled = runtime / "live.pyc"
        return compiled if compiled.is_file() else runtime / "live.py"
    return Path(__file__).resolve().parent / "live.py"


def python_command(script: Path, arguments: list[str]) -> Command:
    """A script, run by this interpreter so it is the same one the backend was built for.

    Through the harness rather than directly, so that whatever the script does with its own host, the
    editor sees the frames.
    """
    return Command(program=interpreter(),
                   arguments=["-u", str(harness()), str(script), *arguments],
                   working_directory=script.parent,
                   environment=script_environment(),
                   encoding="utf-8",
                   frames=True)


def program_command(program: Path, arguments: list[str], working_directory: Path) -> Command:
    # UTF-8, because the compiler is told /utf-8: a narrow string literal in the source comes out of
    # the program as UTF-8 bytes whatever the console's codepage is.
    #
    # `ZL_EDITOR_HOST` says "the editor would like to be your window" (see 宿主/editor_host.h): a
    # program written for it presents into the panel and takes its keys from there, and a program
    # that never looks at the variable behaves exactly as it always did.
    environment = dict(os.environ)
    environment["ZL_EDITOR_HOST"] = "1"
    return Command(program=str(program), arguments=list(arguments),
                   working_directory=working_directory, environment=environment,
                   encoding="utf-8",
                   # stderr carries pictures for a program that took the editor's host. A program
                   # that did not has its words there instead, and the reader hands those back to
                   # the console rather than losing them.
                   frames=True)


class Runner(QObject):
    """Runs one child at a time and reports what it says, and what it shows."""

    output = Signal(str)                    # a line of the child's output
    frame = Signal(bytes, int, int, int)    # rgba, pitch, width, height
    finished = Signal(int, float)           # exit code, seconds

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._process: QProcess | None = None
        self._began = 0.0
        self._buffer = ""
        self._encoding = "utf-8"
        self._reader = frames.Reader()
        self._frames = False

    def running(self) -> bool:
        return self._process is not None

    def process_id(self) -> int | None:
        """The child's process id, so a program's own window can be told apart from everyone else's."""
        if self._process is None:
            return None
        identifier = int(self._process.processId())
        return identifier or None

    def run(self, command: Command) -> None:
        self.stop()

        process = QProcess(self)
        process.setWorkingDirectory(str(command.working_directory))
        if command.environment is not None:
            environment = QProcessEnvironment()
            for key, value in command.environment.items():
                environment.insert(key, value)
            process.setProcessEnvironment(environment)

        self._reader = frames.Reader()
        self._frames = command.frames
        if command.frames:
            # stdout is what the program says, stderr is what it shows.
            process.setProcessChannelMode(QProcess.ProcessChannelMode.SeparateChannels)
            process.readyReadStandardError.connect(self._drain_frames)
        else:
            process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        process.readyReadStandardOutput.connect(self._drain)
        process.finished.connect(self._on_finished)
        process.errorOccurred.connect(self._on_error)

        self._process = process
        self._buffer = ""
        self._encoding = command.encoding
        self._began = time.monotonic()
        process.start(command.program, command.arguments)

    def stop(self) -> None:
        process = self._process
        if process is None:
            return
        self._process = None
        # Which of these were connected depends on how the run was set up -- stderr only carries
        # frames when the program speaks the wire -- so let go of each one that is there.
        for signal in (process.readyReadStandardOutput, process.readyReadStandardError,
                       process.finished, process.errorOccurred):
            try:
                signal.disconnect()
            except (RuntimeError, TypeError):
                pass
        if process.state() != QProcess.ProcessState.NotRunning:
            process.kill()
            process.waitForFinished(2000)

    def write(self, text: str) -> bool:
        """Say something to the child -- a key it should treat as typed. False if it has gone."""
        if self._process is None:
            return False
        return self._process.write(text.encode("utf-8")) >= 0

    # -- internals -------------------------------------------------------------------------

    def _drain(self) -> None:
        assert self._process is not None
        self._take_text(bytes(self._process.readAllStandardOutput())
                        .decode(self._encoding, "replace"))

    def _drain_frames(self) -> None:
        assert self._process is not None
        read = self._reader.feed(bytes(self._process.readAllStandardError()))
        if read.text:
            # Only a program that never looked at `ZL_EDITOR_HOST` puts words on this channel.
            self._take_text(read.text.decode(self._encoding, "replace"))
        for picture in read.frames:
            self.frame.emit(picture.rgba, picture.pitch, picture.width, picture.height)

    def _take_text(self, chunk: str) -> None:
        self._buffer += chunk
        while "\n" in self._buffer:
            line, self._buffer = self._buffer.split("\n", 1)
            self.output.emit(line.rstrip("\r"))

    def _flush(self) -> None:
        if self._buffer:
            self.output.emit(self._buffer.rstrip("\r"))
            self._buffer = ""

    def _on_finished(self, code: int, _status) -> None:
        if self._process is None:
            return
        self._drain()
        if self._frames:
            self._drain_frames()
            left = self._reader.flush()          # a tail of text the child never finished
            if left.text:
                self._take_text(left.text.decode(self._encoding, "replace"))
        self._flush()
        elapsed = time.monotonic() - self._began
        self._process = None
        self.finished.emit(code, elapsed)

    def _on_error(self, error) -> None:
        if self._process is None:
            return
        self.output.emit(f"[子进程起不来: {error}]")
