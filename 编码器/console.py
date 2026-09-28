"""Where a run's output lands, with the tracebacks made clickable.

A Python traceback already says which file and which line; the only thing missing is that saying it
is not the same as being able to go there. So a line that looks like

    File "D:\\...\\scene_demo.py", line 158, in <module>

is written with that span as an anchor, and a click on it hands the path and line back to the
window. Because a Windows path has a colon in it, the anchor carries an opaque key into a table
rather than the path itself.
"""

from __future__ import annotations

import re

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QFont, QTextCharFormat, QTextCursor
from PySide6.QtWidgets import QPlainTextEdit, QWidget

_LOCATION = re.compile(r'File "([^"]+)", line (\d+)')

# How close to the bottom counts as "the user is following along" rather than "scrolled up".
_FOLLOW_SLACK = 4


class Console(QPlainTextEdit):
    jumpRequested = Signal(str, int)   # path, line

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setReadOnly(True)
        self.setMaximumBlockCount(5000)
        self.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
            | Qt.TextInteractionFlag.TextSelectableByKeyboard
        )
        font = QFont("Consolas")
        font.setStyleHint(QFont.StyleHint.Monospace)
        font.setPointSize(10)
        self.setFont(font)
        self._targets: dict[str, tuple[str, int]] = {}

    def clear(self) -> None:
        self._targets.clear()
        super().clear()

    def append_line(self, text: str) -> None:
        scrollbar = self.verticalScrollBar()
        following = scrollbar.value() >= scrollbar.maximum() - _FOLLOW_SLACK

        document = self.document()
        cursor = QTextCursor(document)
        cursor.movePosition(QTextCursor.MoveOperation.End)
        if not document.isEmpty():
            cursor.insertBlock()
        cursor.insertText(text)

        block_start = cursor.block().position()
        for match in _LOCATION.finditer(text):
            key = str(len(self._targets))
            self._targets[key] = (match.group(1), int(match.group(2)))
            span = QTextCursor(document)
            span.setPosition(block_start + match.start())
            span.setPosition(block_start + match.end(), QTextCursor.MoveMode.KeepAnchor)
            link = QTextCharFormat()
            link.setAnchor(True)
            link.setAnchorHref("zlong://" + key)
            link.setForeground(QColor("#4ec9b0"))
            link.setFontUnderline(True)
            span.mergeCharFormat(link)

        # Only follow the tail if the user had not gone looking further up.
        if following:
            scrollbar.setValue(scrollbar.maximum())

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802 - Qt's name
        super().mouseReleaseEvent(event)
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self.textCursor().hasSelection():
            return   # a drag to select, not a click on a link

        position = event.position().toPoint()
        href = self.cursorForPosition(position).charFormat().anchorHref()
        if not href.startswith("zlong://"):
            return
        target = self._targets.get(href.split("//", 1)[1])
        if target is not None:
            self.jumpRequested.emit(*target)
