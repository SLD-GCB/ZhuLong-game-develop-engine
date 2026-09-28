"""The panel on the right: whatever the running program presented, as it presented it.

It holds one frame -- the newest -- and paints it scaled to fit, keeping its shape. Nothing is
animated and nothing is queued: a game that presents slower than it runs would rather the editor
showed the last thing it managed than a growing backlog, and a program that has finished still has
its last frame on screen, which is often the point of running it.

It is also where a program's keyboard goes. A program that presented into the panel instead of
opening a window of its own has no other way to be typed at, so this widget takes focus and hands
every key on as the virtual-key code the program would have got from a window -- see `window.py` for
the other kind of program, the one that insists on having a window.

The **mouse** goes the same way, and one thing about it is decided here rather than there: the
position is sent in the **frame's** pixels, not the panel's. The panel scales the picture to fit and
centres it, and only the panel knows by how much -- so a program that gets `(x, y)` can divide by the
frame size it rendered at and have a ray, without ever being told about letterboxing. A point outside
the picture is not sent at all.
"""

from __future__ import annotations

from PySide6.QtCore import QRect, QPoint, Qt, Signal
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtWidgets import QSizePolicy, QWidget

# What the panel says before anything has run.
_EMPTY = "运行后这里显示画面"

# And what it says when it is holding a program's keyboard but does not have it yet.
_UNFOCUSED = "点一下这里，键盘和鼠标就进程序"

_MARGIN = 6
_FOCUS_COLOUR = "#4ec9b0"

# Qt's button to the number the wire carries. They are Windows' own masks -- MK_LBUTTON is 1,
# MK_RBUTTON 2, MK_MBUTTON 4 -- so nothing on either side has to translate twice.
_BUTTONS = {
    Qt.MouseButton.LeftButton: 1,
    Qt.MouseButton.RightButton: 2,
    Qt.MouseButton.MiddleButton: 4,
}

# One notch of a wheel. Qt reports eighths of a degree, and a notch is 120 of them.
_NOTCH = 120

# Qt's modifiers to the number the wire carries: 1 Shift, 2 Ctrl, 4 Alt. The same little set of bits
# the mouse buttons use, so the whole protocol has one way of saying "what else was held".
_MODIFIERS = (
    (Qt.KeyboardModifier.ShiftModifier, 1),
    (Qt.KeyboardModifier.ControlModifier, 2),
    (Qt.KeyboardModifier.AltModifier, 4),
)


def _held_modifiers(event) -> int:
    mask = 0
    for flag, bit in _MODIFIERS:
        if event.modifiers() & flag:
            mask |= bit
    return mask


class Viewport(QWidget):
    """Shows the newest frame, and passes the keyboard and the mouse on when a program is listening."""

    key = Signal(int, bool, int)         # a virtual-key code, whether it went down, what was held
    moved = Signal(int, int)             # pointer, in the frame's pixels
    clicked = Signal(int, bool, int, int)  # button, whether it went down, and where (-1 when
                                           # the click was outside the picture)
    wheeled = Signal(int)                # notches, away from the user positive

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._image: QImage | None = None
        self._frames = 0
        self._capturing = False
        self.setMinimumSize(240, 160)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self.setAutoFillBackground(False)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        # Windows only sends move events while a button is held unless the widget asks for the rest.
        self.setMouseTracking(True)

    @property
    def frames(self) -> int:
        """How many frames have arrived. What the panel is showing is the last of them."""
        return self._frames

    def size_of_frame(self) -> tuple[int, int]:
        return (self._image.width(), self._image.height()) if self._image is not None else (0, 0)

    def set_capturing(self, on: bool) -> None:
        """Whether a running program is waiting to be typed at."""
        self._capturing = on
        if not on and self.hasFocus():
            self.clearFocus()
        self.update()

    def capturing(self) -> bool:
        return self._capturing

    def show_frame(self, rgba: bytes, pitch: int, width: int, height: int) -> None:
        # A copy, because `rgba` is a buffer that belongs to the child's message and is gone after
        # this call; QImage does not take ownership of what it is handed.
        self.show_image(QImage(rgba, width, height, pitch, QImage.Format.Format_RGBA8888))

    def show_image(self, image: QImage) -> None:
        """Show a picture that is already a QImage -- the copy taken off a program's own window."""
        self._image = image.copy()
        self._frames += 1
        self.update()

    def clear(self) -> None:
        """Forget the last run's picture, so the next one cannot be mistaken for it."""
        self._image = None
        self._frames = 0
        self.update()

    # -- where the picture landed ------------------------------------------------------------

    def room(self) -> QRect:
        """图能占的那些像素 —— 一圈边距去掉之后剩下的。

        A program that wants its picture to **fill** the panel renders at this shape; one that
        renders at some fixed size instead gets letterboxed, and on a wide panel that is two black
        bars and a picture that looks smaller than the window it is in.
        """
        return self.rect().adjusted(_MARGIN, _MARGIN, -_MARGIN, -_MARGIN)

    def frame_area(self) -> QRect | None:
        """The rectangle the picture is drawn in, or None when there is nothing to draw.

        One definition, used by both the painter and the pointer: a mouse position means nothing
        unless it is measured against the same rectangle the picture was put in.
        """
        if self._image is None or self._image.isNull():
            return None
        room = self.room()
        if room.isEmpty():
            return None
        scaled = self._image.size().scaled(room.size(), Qt.AspectRatioMode.KeepAspectRatio)
        left = room.x() + (room.width() - scaled.width()) // 2
        top = room.y() + (room.height() - scaled.height()) // 2
        return QRect(left, top, scaled.width(), scaled.height())

    def frame_at(self, point: QPoint) -> tuple[int, int] | None:
        """A point in this widget, as a pixel of the frame -- or None when it is outside it.

        Outside is not clamped to the nearest edge: a drag that wandered off the picture and came
        back would otherwise look like it had slid along the border.

        The arithmetic is on the **centre** of the panel pixel rather than its corner --
        `(2*offset + 1) * size // (2 * span)` is that, in integers. Taking the corner instead
        (`offset * size // span`) reads the left edge of a pixel as its whole extent, which is off
        by one wherever the ratio does not divide, and leaves the last row and column of the frame
        unreachable -- so a click in the very corner of the picture missed it.
        """
        area = self.frame_area()
        if area is None or not area.contains(point):
            return None
        assert self._image is not None
        x = ((2 * (point.x() - area.x()) + 1) * self._image.width()) // (2 * area.width())
        y = ((2 * (point.y() - area.y()) + 1) * self._image.height()) // (2 * area.height())
        return (min(x, self._image.width() - 1), min(y, self._image.height() - 1))

    # -- the keyboard ----------------------------------------------------------------------

    def keyPressEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if self._capturing and event.nativeVirtualKey():
            # Auto-repeat is the key being held, not the key being pressed again. The window this
            # replaces made the same distinction, and the game was told about it.
            if not event.isAutoRepeat():
                self.key.emit(int(event.nativeVirtualKey()), True, _held_modifiers(event))
            event.accept()
            return
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if self._capturing and event.nativeVirtualKey() and not event.isAutoRepeat():
            # The release is sent too: "the shift went up" is only sayable on the way up, and a
            # modifier that never comes up is a modifier that is stuck on.
            self.key.emit(int(event.nativeVirtualKey()), False, _held_modifiers(event))
            event.accept()
            return
        super().keyReleaseEvent(event)

    # -- the mouse -------------------------------------------------------------------------

    def mouseMoveEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if self._capturing:
            at = self.frame_at(event.position().toPoint())
            if at is not None:
                self.moved.emit(*at)
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mousePressEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if self._capturing:
            self.setFocus(Qt.FocusReason.MouseFocusReason)
            at = self.frame_at(event.position().toPoint())
            if at is not None:
                # Keep the pointer state fresh as well: a drag turns or pans by the *difference*
                # between moves, and without this the first move after a click would be measured
                # from wherever the pointer was before the click.
                self.moved.emit(*at)
            self._emit_click(event.button(), True, at)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if self._capturing:
            at = self.frame_at(event.position().toPoint())
            if at is not None:
                self.moved.emit(*at)
            self._emit_click(event.button(), False, at)
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def _emit_click(self, button, down: bool, at: tuple[int, int] | None) -> None:
        """A button action, on its way out -- with where it happened.

        The position is **carried in the event**, not left for the program to read off the pointer.
        A press and a move can land in the same frame, and then "where is the pointer" is a different
        question from "where was the button pressed" -- answering the wrong one makes a drag lose its
        first movement and land short of where the hand went.

        Outside the picture there is no frame pixel to name, so -1 says so and the program falls back
        to the pointer. Emitting anyway is the point: a drag that wanders off the edge and comes back
        still has to be told the button came up.
        """
        code = _BUTTONS.get(button)
        if code is None:
            return
        if at is None:
            self.clicked.emit(code, down, -1, -1)
        else:
            self.clicked.emit(code, down, at[0], at[1])

    def wheelEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if self._capturing:
            notches = int(event.angleDelta().y()) // _NOTCH
            if notches:
                self.wheeled.emit(notches)
            event.accept()
            return
        super().wheelEvent(event)

    def focusInEvent(self, event) -> None:  # noqa: N802 - Qt's name
        super().focusInEvent(event)
        self.update()

    def focusOutEvent(self, event) -> None:  # noqa: N802 - Qt's name
        super().focusOutEvent(event)
        self.update()

    # -- painting --------------------------------------------------------------------------

    def paintEvent(self, _event) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#141414"))

        if self._image is None:
            painter.setPen(QColor("#6e7681"))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, _EMPTY)
            return

        area = self.frame_area()
        if area is not None:
            # Centre it: a frame's shape is the program's business, not the panel's.
            painter.drawImage(
                area,
                self._image,
                QRect(0, 0, self._image.width(), self._image.height()),
            )

        if not self._capturing:
            return
        if self.hasFocus():
            # A line round the edge, so it is obvious which half of the window is listening.
            painter.setPen(QColor(_FOCUS_COLOUR))
            painter.drawRect(self.rect().adjusted(0, 0, -1, -1))
        else:
            painter.setPen(QColor("#d4d4d4"))
            band = QRect(0, self.height() - 26, self.width(), 26)
            painter.fillRect(band, QColor(0, 0, 0, 150))
            painter.drawText(band, Qt.AlignmentFlag.AlignCenter, _UNFOCUSED)
