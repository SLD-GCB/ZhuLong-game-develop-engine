"""Mirroring a running program's own window into the panel.

A C++ host presents where it was written to present -- the chess game draws into a GDI window of its
own -- and nothing outside can redirect that: `Platform::Present` is a virtual call inside a compiled
program. What the editor *can* do is look at it. So while a program runs, the editor watches for a
top-level window belonging to it and copies what is on that window into the panel on the right.

The program keeps its window, and that is the point rather than an oversight: the window is where its
keyboard goes. The panel is a copy.

`PrintWindow` rather than a grab off the screen, because the editor is usually in front of the thing
it is watching -- reading the screen would show the editor. That means the size needs reasoning about:
a program that has not declared itself DPI aware has its client area stretched on screen, and what
comes back from the window is the *unstretched* version. So the size Windows reports is divided by
the screen's scale, and the picture is the program's own pixels at the program's own resolution.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wintypes
import time
from dataclasses import dataclass

from PySide6.QtCore import QObject, QTimer, Signal
from PySide6.QtGui import QGuiApplication, QImage

_user32 = ctypes.windll.user32
_gdi32 = ctypes.windll.gdi32

_PW_RENDERFULLCONTENT = 0x00000002

# The smallest thing worth calling a program's window, so that hidden helper windows and message-only
# windows are not mistaken for it.
_MINIMUM = 64

# How long to keep looking for a window before deciding there will not be one, and how long to keep
# asking after one has gone away in case another is coming.
_PATIENCE = 15.0
_LOST_AFTER = 1.0

_PROCESS_DPI_UNAWARE = 0
_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


class _BitmapInfoHeader(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class _BitmapInfo(ctypes.Structure):
    _fields_ = [("bmiHeader", _BitmapInfoHeader), ("bmiColors", wintypes.DWORD * 3)]


@dataclass(frozen=True)
class Target:
    """A program's window, measured in the program's own pixels.

    `left`/`top` are where its client area sits inside the window, because `PrintWindow` draws the
    window from its own origin -- title bar and borders included -- and only the client is the
    picture.
    """

    window: int
    width: int
    height: int
    left: int = 0
    top: int = 0


def find(pid: int) -> Target | None:
    """The first visible top-level window of `pid` that is big enough to be the program's own."""
    found: list[int] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _parameter):
        owner = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not _user32.IsWindowVisible(hwnd):
            return True
        if _user32.GetWindow(hwnd, 4):          # GW_OWNER: a dialog or a tool window, not the program
            return True
        if _reported_client_size(hwnd)[0] < _MINIMUM * 2:
            return True
        found.append(hwnd)
        return False                            # the first one in z-order is the program's window

    _user32.EnumWindows(visit, 0)
    if not found:
        return None

    hwnd = found[0]
    scale = _stretch(pid)
    width, height = _unscaled(_reported_client_size(hwnd), scale)
    left, top = _unscaled(_reported_client_offset(hwnd), scale)
    return Target(window=hwnd, width=width, height=height, left=left, top=top)


def capture(target: Target) -> QImage | None:
    """What is on that window right now, at the size the program itself draws at."""
    if not target.window:
        # Window 0 is the screen, not a window: asking for its DC would succeed and answer with a
        # picture of the desktop.
        return None

    wide = target.left + target.width
    tall = target.top + target.height
    window_dc = _user32.GetDC(target.window)
    if not window_dc:
        return None
    memory_dc = _gdi32.CreateCompatibleDC(window_dc)
    bitmap = _gdi32.CreateCompatibleBitmap(window_dc, wide, tall)
    if not memory_dc or not bitmap:
        _user32.ReleaseDC(target.window, window_dc)
        return None

    try:
        _gdi32.SelectObject(memory_dc, bitmap)
        _user32.PrintWindow(target.window, memory_dc, _PW_RENDERFULLCONTENT)

        info = _BitmapInfo()
        info.bmiHeader.biSize = ctypes.sizeof(_BitmapInfoHeader)
        info.bmiHeader.biWidth = wide
        info.bmiHeader.biHeight = -tall           # negative: the top row comes first
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        info.bmiHeader.biCompression = 0          # BI_RGB

        buffer = ctypes.create_string_buffer(wide * tall * 4)
        _gdi32.GetDIBits(memory_dc, bitmap, 0, tall, buffer, ctypes.byref(info), 0)
        # A DIB is BGRA in memory, which is what Format_RGB32 reads.
        whole = QImage(buffer.raw, wide, tall, wide * 4, QImage.Format.Format_RGB32).copy()
        return whole.copy(target.left, target.top, target.width, target.height)
    finally:
        _gdi32.DeleteObject(bitmap)
        _gdi32.DeleteDC(memory_dc)
        _user32.ReleaseDC(target.window, window_dc)


def _reported_client_size(hwnd) -> tuple[int, int]:
    rect = wintypes.RECT()
    _user32.GetClientRect(hwnd, ctypes.byref(rect))
    return rect.right - rect.left, rect.bottom - rect.top


def _reported_client_offset(hwnd) -> tuple[int, int]:
    window = wintypes.RECT()
    _user32.GetWindowRect(hwnd, ctypes.byref(window))
    origin = wintypes.POINT(0, 0)
    _user32.ClientToScreen(hwnd, ctypes.byref(origin))
    return origin.x - window.left, origin.y - window.top


def _unscaled(pair: tuple[int, int], scale: float) -> tuple[int, int]:
    return max(0, int(pair[0] / scale)), max(0, int(pair[1] / scale))


def _stretch(pid: int) -> float:
    """The factor by which Windows stretches this program's window, or 1.0 if it stretches nothing.

    A program that has not declared itself DPI aware is given a stretched copy of its window on
    screen, and a DPI-aware reader -- which this is, because Qt says so -- is told the stretched
    measurements. But `PrintWindow` hands back the program's own unstretched pixels, and everything
    about it is unstretched: the client area, and where that area sits inside the window. So one
    factor divides the lot, and the picture is the program's at the program's own resolution rather
    than a blown-up copy of it.
    """
    if not _is_dpi_unaware(pid):
        return 1.0
    screen = QGuiApplication.primaryScreen()
    scale = screen.devicePixelRatio() if screen is not None else 1.0
    return scale if scale > 0 else 1.0


def _is_dpi_unaware(pid: int) -> bool:
    handle = ctypes.windll.kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    try:
        awareness = ctypes.c_int()
        # Windows 8.1 and later. On anything older the call is missing, and assuming awareness is
        # the safer way to be wrong: it just means an unstretched picture with a margin.
        if not hasattr(ctypes.windll.shcore, "GetProcessDpiAwareness"):
            return False
        result = ctypes.windll.shcore.GetProcessDpiAwareness(handle, ctypes.byref(awareness))
        return result == 0 and awareness.value == _PROCESS_DPI_UNAWARE
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


class Mirror(QObject):
    """Watches a running program for a window, and hands back what is on it."""

    frame = Signal(QImage)

    def __init__(self, parent: QObject | None = None, interval_ms: int = 100,
                 patience: float = _PATIENCE) -> None:
        super().__init__(parent)
        self._target: Target | None = None
        self._deadline = 0.0
        self._lost_at: float | None = None
        self._patience = patience
        self._pid: int | None = None
        self._timer = QTimer(self)
        self._timer.setInterval(interval_ms)
        self._timer.timeout.connect(self._tick)

    def watch(self, pid: int | None) -> None:
        self.stop()
        if pid is None:
            return
        self._pid = pid
        self._target = None
        self._lost_at = None
        self._deadline = time.monotonic() + self._patience
        self._timer.start()

    def stop(self) -> None:
        self._timer.stop()
        self._target = None
        self._pid = None

    def watching(self) -> bool:
        return self._timer.isActive()

    def _tick(self) -> None:
        if self._target is None:
            self._target = find(self._pid)
            if self._target is None:
                # A program that opens no window will never open one. Asking `EnumWindows` ten times
                # a second for ever is not patience, it is a waste.
                if time.monotonic() > self._deadline:
                    self.stop()
                return

        picture = capture(self._target)
        if picture is None or picture.isNull():
            # The window went away: the program closed it. Ask for a little while in case another
            # one is coming, then let it go.
            if self._lost_at is None:
                self._lost_at = time.monotonic()
            elif time.monotonic() - self._lost_at > _LOST_AFTER:
                self.stop()
            return

        self._lost_at = None
        self.frame.emit(picture)
