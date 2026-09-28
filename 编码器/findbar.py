"""Find and replace, as a bar that sits over the editor rather than a dialog over the window.

A dialog steals the focus and has to be dismissed; a bar is part of the editor, which is what makes
Ctrl+F feel like it belongs to the text. The bar does not do the searching itself -- `QTextDocument`
already searches, with regexes, forwards, backwards and wrapping -- so what is here is the one
pattern all four modes reduce to, and the bookkeeping around it.
"""

from __future__ import annotations

import re

from PySide6.QtCore import QRegularExpression, Qt, Signal
from PySide6.QtGui import QTextCursor, QTextDocument
from PySide6.QtWidgets import (
    QCheckBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPlainTextEdit,
    QPushButton,
    QWidget,
)

# `\1` in a replacement, meaning the first capture group. Qt substitutes these in
# `QString::replace` but not when a document search hands back a cursor, so it is done here.
_GROUP = re.compile(r"\\(\d)")


class FindBar(QWidget):
    """Search the editor it is given. `matches` carries every hit so the editor can draw them."""

    matches = Signal(list)     # [(start, length), ...]
    closed = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._editor: QPlainTextEdit | None = None
        self._current = -1
        self._spans: list[tuple[int, int]] = []

        self._search = QLineEdit()
        self._search.setPlaceholderText("查找")
        self._search.setClearButtonEnabled(True)
        self._replace = QLineEdit()
        self._replace.setPlaceholderText("替换为")

        self._case = QCheckBox("Aa")
        self._case.setToolTip("区分大小写")
        self._word = QCheckBox("词")
        self._word.setToolTip("全字匹配")
        self._regex = QCheckBox(".*")
        self._regex.setToolTip("正则表达式")

        self._previous = QPushButton("上一个")
        self._next = QPushButton("下一个")
        self._replace_one = QPushButton("替换")
        self._replace_all = QPushButton("全部替换")
        self._status = QLabel("")
        self._status.setMinimumWidth(60)
        self._close = QPushButton("✕")
        self._close.setFixedWidth(28)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(6, 3, 6, 3)
        layout.setSpacing(5)
        for widget in (self._search, self._replace):
            widget.setMinimumWidth(150)
            layout.addWidget(widget)
        layout.addWidget(self._status)
        layout.addStretch(1)
        for widget in (self._case, self._word, self._regex, self._previous, self._next,
                       self._replace_one, self._replace_all, self._close):
            layout.addWidget(widget)

        self.setStyleSheet(
            "QLineEdit { background:#3c3c3c; color:#d4d4d4; border:1px solid #3c3c3c;"
            " padding:3px 6px; }"
            "QCheckBox, QLabel { color:#9da5b4; }"
            "QPushButton { background:#2d2d2d; color:#d4d4d4; border:1px solid #3c3c3c;"
            " padding:3px 8px; }"
        )

        self._search.textChanged.connect(self._on_changed)
        self._search.returnPressed.connect(lambda: self.find_next(False))
        self._replace.returnPressed.connect(self.replace_current)
        for box in (self._case, self._word, self._regex):
            box.toggled.connect(self._on_changed)
        self._next.clicked.connect(lambda: self.find_next(False))
        self._previous.clicked.connect(lambda: self.find_next(True))
        self._replace_one.clicked.connect(self.replace_current)
        self._replace_all.clicked.connect(self.replace_all)
        self._close.clicked.connect(self.hide_bar)

    # -- what the window calls -------------------------------------------------------------

    def open_on(self, editor: QPlainTextEdit, replace: bool = False) -> None:
        if self._editor is not editor:
            self._editor = editor
            self._current = -1
            self._spans = []
        self._replace.setVisible(replace)
        self._replace_one.setVisible(replace)
        self._replace_all.setVisible(replace)
        self.show()

        cursor = editor.textCursor()
        if cursor.hasSelection():
            # Search for what was picked, the way Ctrl+F is expected to behave.
            self._search.setText(cursor.selectedText().replace("\u2029", "\n"))
        self._search.setFocus()
        self._search.selectAll()
        self.refresh()
        if self._search.text():
            self.find_next(False)

    @property
    def target(self) -> QPlainTextEdit | None:
        return self._editor

    @property
    def replacing(self) -> bool:
        return self._replace.isVisible()

    def hide_bar(self) -> None:
        self.matches.emit([])
        self._current = -1
        self.hide()
        if self._editor is not None:
            self._editor.setFocus()
        self.closed.emit()

    def keyPressEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if event.key() == Qt.Key.Key_Escape:
            self.hide_bar()
            return
        if event.key() == Qt.Key.Key_F3:
            self.find_next(bool(event.modifiers() & Qt.KeyboardModifier.ShiftModifier))
            return
        super().keyPressEvent(event)

    # -- the searching ---------------------------------------------------------------------

    def pattern(self) -> QRegularExpression | None:
        text = self._search.text()
        if not text:
            return None
        # Plain searches become regexes too, escaped. One code path instead of two, and the
        # not-regex case is exactly the regex that matches itself.
        expression = text if self._regex.isChecked() else QRegularExpression.escape(text)
        if self._word.isChecked():
            expression = rf"\b(?:{expression})\b"
        pattern = QRegularExpression(expression)
        if not pattern.isValid():
            return None
        if not self._case.isChecked():
            pattern.setPatternOptions(QRegularExpression.PatternOption.CaseInsensitiveOption)
        return pattern

    def search_flags(self, backward: bool = False) -> QTextDocument.FindFlags:
        """How to search.

        Case is passed as a flag and not left to the pattern's own options, because
        `QTextDocument.find` decides case sensitivity from the flag alone -- a search for a
        case-sensitive pattern with no flag still matches "ALPHA" against "alpha". The option
        stays on the pattern as well so that `match()`, which does respect it, agrees.
        """
        flags = QTextDocument.FindFlag(0)
        if self._case.isChecked():
            flags |= QTextDocument.FindFlag.FindCaseSensitively
        if backward:
            flags |= QTextDocument.FindFlag.FindBackward
        return flags

    def span_of(self, cursor: QTextCursor) -> tuple[int, int]:
        return cursor.selectionStart(), cursor.selectionEnd() - cursor.selectionStart()

    def all_matches(self) -> list[tuple[int, int]]:
        pattern = self.pattern()
        if pattern is None or self._editor is None:
            return []
        found: list[tuple[int, int]] = []
        document = self._editor.document()
        cursor = QTextCursor(document)
        flags = self.search_flags()
        guard = document.characterCount() + 2
        while len(found) < guard:
            cursor = document.find(pattern, cursor, flags)
            if cursor.isNull():
                break
            span = self.span_of(cursor)
            if span[1] == 0:
                break   # a pattern that can match nothing would spin forever
            found.append(span)
        return found

    def find_next(self, backward: bool = False) -> None:
        if self._editor is None:
            return
        pattern = self.pattern()
        if pattern is None:
            self._spans = []
            self.matches.emit([])
            self._status.setText("")
            return

        flag = self.search_flags(backward)
        document = self._editor.document()
        cursor = self._editor.textCursor()
        if cursor.hasSelection():
            # Start from the far edge of what is already found, so "next" moves on.
            cursor.setPosition(cursor.selectionStart() if backward else cursor.selectionEnd())

        found = document.find(pattern, cursor, flag)
        if found.isNull():
            # Wrap around, which is what everything that is not a form does.
            wrap = QTextCursor(document)
            if backward:
                wrap.movePosition(QTextCursor.MoveOperation.End)
            found = document.find(pattern, wrap, flag)
        if found.isNull():
            # Drop the last hit's selection too, or a search with no results leaves something
            # looking selected that no longer matches.
            stale = self._editor.textCursor()
            stale.clearSelection()
            self._editor.setTextCursor(stale)
            self._spans = []
            self.matches.emit([])
            self._status.setText("无匹配")
            return

        self._editor.setTextCursor(found)
        self.refresh()
        span = self.span_of(found)
        self._current = self._spans.index(span) if span in self._spans else -1
        self._update_status()

    def replace_current(self) -> None:
        if self._editor is None:
            return
        pattern = self.pattern()
        cursor = self._editor.textCursor()
        if pattern is not None and cursor.hasSelection():
            selected = cursor.selectedText().replace("\u2029", "\n")
            match = pattern.match(selected)
            if match.hasMatch() and match.capturedLength() == len(selected):
                cursor.insertText(self._expand(self._replace.text(), match))
                self._editor.setTextCursor(cursor)
        self.find_next(False)

    def replace_all(self) -> None:
        pattern = self.pattern()
        if pattern is None or self._editor is None:
            return
        document = self._editor.document()
        flags = self.search_flags()
        cursor = QTextCursor(document)
        cursor.beginEditBlock()
        replaced = 0
        while True:
            found = document.find(pattern, cursor, flags)
            if found.isNull() or self.span_of(found)[1] == 0:
                break
            matched = found.selectedText().replace("\u2029", "\n")
            found.insertText(self._expand(self._replace.text(), pattern.match(matched)))
            cursor = found
            replaced += 1
        cursor.endEditBlock()
        self.refresh()
        self._status.setText(f"替换 {replaced}")

    @staticmethod
    def _expand(template: str, match) -> str:
        return _GROUP.sub(lambda found: match.captured(int(found.group(1))), template)

    # -- bookkeeping -----------------------------------------------------------------------

    def refresh(self) -> None:
        """Recount, redraw every hit, and put the counter back in step with the document."""
        self._spans = self.all_matches()
        self.matches.emit(self._spans)
        if self._current >= len(self._spans):
            self._current = -1
        self._update_status()

    def _on_changed(self) -> None:
        self._current = -1
        if self._search.text():
            self.find_next(False)
        else:
            self.refresh()

    def _update_status(self) -> None:
        total = len(self._spans)
        if not self._search.text():
            self._status.setText("")
        elif total == 0:
            self._status.setText("无匹配")
        elif self._current >= 0:
            self._status.setText(f"{self._current + 1}/{total}")
        else:
            self._status.setText(f"{total}")
