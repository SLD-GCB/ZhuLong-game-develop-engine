"""The text surface itself: a plain text editor with a line-number gutter, the editing manners a
programmer expects, and highlighting.

There is nothing clever here on purpose. The editor edits text; the interesting thing about this one
is that what it edits can drive the machine — a Python script through `import zlong`, or a C++ file
compiled and linked against the libraries in `build/`.

The manners are the usual set: Return indents, Tab indents a block or jumps to the next stop,
brackets and quotes close themselves, the bracket under the cursor and its partner are marked,
Ctrl+/ comments, Alt+Up/Down moves lines, Ctrl+D duplicates them. All of it is text surgery on the
current block(s) -- there is no parser behind it.
"""

from __future__ import annotations

from PySide6.QtCore import QRect, QSize, Qt
from PySide6.QtGui import QColor, QFont, QFontDatabase, QPainter, QTextCharFormat, QTextCursor, QTextFormat
from PySide6.QtWidgets import QPlainTextEdit, QTextEdit, QWidget

from highlighter import CppHighlighter, PythonHighlighter

INDENT = 4

# What a typed character opens, and the set of characters that can close something.
_AUTO_PAIRS = {"(": ")", "[": "]", "{": "}", '"': '"', "'": "'"}
_BRACKETS = {"(": ")", "[": "]", "{": "}", ")": "(", "]": "[", "}": "{"}
_QUOTES = {'"', "'"}

# What opens a block, per language: a colon ends a Python line that wants an indented body, a brace
# opens one in C++ (and in a Python dict literal, where indenting is what you would do anyway).
_OPENS_A_BLOCK = {":", "{"}

# What a comment starts with.
_COMMENT = {"python": "#", "cpp": "//"}

_GUTTER_MARGIN = 4


class _LineNumberArea(QWidget):
    def __init__(self, editor: "CodeEditor") -> None:
        super().__init__(editor)
        self._editor = editor

    def sizeHint(self) -> QSize:  # noqa: N802 - Qt's name
        return QSize(self._editor.gutter_width(), 0)

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt's name
        self._editor.paint_gutter(event)


class CodeEditor(QPlainTextEdit):
    """A `QPlainTextEdit` that knows its line numbers, its language, and its manners."""

    def __init__(self, parent: QWidget | None = None, language: str = "python") -> None:
        super().__init__(parent)

        self.language = language
        self._gutter = _LineNumberArea(self)
        self.language = "text"
        self._highlighter = None
        self._search_spans: list[tuple[int, int]] = []

        font = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        font.setPointSize(11)
        font.setStyleHint(QFont.StyleHint.Monospace)
        self.setFont(font)
        self.setTabStopDistance(INDENT * self.fontMetrics().horizontalAdvance(" "))
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)

        self.blockCountChanged.connect(self._update_gutter_width)
        self.updateRequest.connect(self._update_gutter)
        self.cursorPositionChanged.connect(self.refresh_highlights)

        self.set_language(language)
        self._update_gutter_width(0)
        self.refresh_highlights()

    # -- what the file is ------------------------------------------------------------------

    def set_language(self, language: str) -> None:
        """Treat this file as `python`, `cpp` or plain `text`.

        Three things follow from this and nothing else does: how the text is coloured, what a
        comment starts with, and which half of Run the file goes to. That is why it can be changed
        while the file is open -- a file is not what its name says it is, it is what it is being
        treated as.
        """
        if language == self.language and self._highlighter is not None:
            return
        self.language = language

        if self._highlighter is not None:
            # Detach before dropping the reference, or the old one keeps colouring the document.
            self._highlighter.setDocument(None)
            self._highlighter = None

        if language == "python":
            self._highlighter = PythonHighlighter(self.document())
        elif language == "cpp":
            self._highlighter = CppHighlighter(self.document())
        if self._highlighter is not None:
            self._highlighter.rehighlight()
        self.refresh_highlights()

    # -- the gutter ------------------------------------------------------------------------

    def gutter_width(self) -> int:
        digits = max(3, len(str(max(1, self.blockCount()))))
        return _GUTTER_MARGIN * 2 + self.fontMetrics().horizontalAdvance("9") * digits

    def _update_gutter_width(self, _block_count: int) -> None:
        self.setViewportMargins(self.gutter_width(), 0, 0, 0)

    def _update_gutter(self, rect: QRect, dy: int) -> None:
        if dy:
            self._gutter.scroll(0, dy)
        else:
            self._gutter.update(0, rect.y(), self._gutter.width(), rect.height())
        if rect.contains(self.viewport().rect()):
            self._update_gutter_width(0)

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt's name
        super().resizeEvent(event)
        area = self.contentsRect()
        self._gutter.setGeometry(
            QRect(area.left(), area.top(), self.gutter_width(), area.height())
        )

    def paint_gutter(self, event) -> None:
        painter = QPainter(self._gutter)
        painter.fillRect(event.rect(), QColor("#1e1e1e"))

        block = self.firstVisibleBlock()
        number = block.blockNumber()
        top = self.blockBoundingGeometry(block).translated(self.contentOffset()).top()
        bottom = top + self.blockBoundingRect(block).height()
        width = self._gutter.width() - _GUTTER_MARGIN
        height = self.fontMetrics().height()

        current = self.textCursor().blockNumber()
        while block.isValid() and top <= event.rect().bottom():
            if block.isVisible() and bottom >= event.rect().top():
                painter.setPen(QColor("#d4d4d4") if number == current else QColor("#6e7681"))
                painter.drawText(
                    0,
                    int(top),
                    width,
                    height,
                    Qt.AlignmentFlag.AlignRight,
                    str(number + 1),
                )
            block = block.next()
            top = bottom
            bottom = top + self.blockBoundingRect(block).height()
            number += 1

    # -- what is drawn behind the text -----------------------------------------------------

    def set_search_spans(self, spans: list[tuple[int, int]]) -> None:
        """The find bar's matches, so every hit is visible and not only the current one."""
        self._search_spans = list(spans)
        self.refresh_highlights()

    def refresh_highlights(self) -> None:
        selections: list[QTextEdit.ExtraSelection] = []

        band = QTextEdit.ExtraSelection()
        band.format.setBackground(QColor("#2a2d2e"))
        band.format.setProperty(QTextFormat.Property.FullWidthSelection, True)
        band.cursor = self.textCursor()
        band.cursor.clearSelection()
        selections.append(band)

        for start, length in self._search_spans:
            if length <= 0:
                continue
            selections.append(self._span(start, length, QColor("#42341a"), None))

        for start, length in self._bracket_spans():
            selections.append(self._span(start, length, QColor("#3a3d41"), QColor("#ffd866")))

        self.setExtraSelections(selections)

    def _span(self, start: int, length: int, background: QColor, foreground: QColor | None):
        selection = QTextEdit.ExtraSelection()
        selection.format.setBackground(background)
        if foreground is not None:
            selection.format.setForeground(foreground)
        cursor = QTextCursor(self.document())
        cursor.setPosition(start)
        cursor.setPosition(start + length, QTextCursor.MoveMode.KeepAnchor)
        selection.cursor = cursor
        return selection

    def _bracket_spans(self) -> list[tuple[int, int]]:
        text = self.toPlainText()
        position = self.textCursor().position()
        for probe in (position - 1, position):
            if 0 <= probe < len(text) and text[probe] in _BRACKETS:
                partner = self._matching_bracket(text, probe)
                if partner is not None:
                    return [(probe, 1), (partner, 1)]
        return []

    @staticmethod
    def _matching_bracket(text: str, index: int) -> int | None:
        char = text[index]
        partner = _BRACKETS[char]
        if char in "([{":
            depth = 0
            for step in range(index, len(text)):
                if text[step] == char:
                    depth += 1
                elif text[step] == partner:
                    depth -= 1
                    if depth == 0:
                        return step
        else:
            depth = 0
            for step in range(index, -1, -1):
                if text[step] == char:
                    depth += 1
                elif text[step] == partner:
                    depth -= 1
                    if depth == 0:
                        return step
        return None

    # -- placement and zoom ----------------------------------------------------------------

    def goto_line(self, line: int) -> None:
        # Clamped rather than refused: a jump that overshoots the file should land on its last line,
        # which is where you would have scrolled to anyway.
        number = max(0, min(line - 1, self.blockCount() - 1))
        block = self.document().findBlockByNumber(number)
        if not block.isValid():
            return
        cursor = QTextCursor(block)
        self.setTextCursor(cursor)
        self.centerCursor()
        self.setFocus()

    def font_point_size(self) -> int:
        return self.font().pointSize()

    def set_font_point_size(self, size: int) -> None:
        size = max(6, min(48, size))
        font = self.font()
        if font.pointSize() == size:
            return
        font.setPointSize(size)
        self.setFont(font)
        self.setTabStopDistance(INDENT * self.fontMetrics().horizontalAdvance(" "))
        self._update_gutter_width(0)
        self.refresh_highlights()

    # -- the manners -----------------------------------------------------------------------

    def keyPressEvent(self, event) -> None:  # noqa: N802 - Qt's name
        key = event.key()
        control = bool(event.modifiers() & Qt.KeyboardModifier.ControlModifier)
        alt = bool(event.modifiers() & Qt.KeyboardModifier.AltModifier)

        if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter) and not control:
            self._insert_newline()
            return
        if key == Qt.Key.Key_Tab and not control:
            self._handle_tab()
            return
        if key == Qt.Key.Key_Backtab:
            self._shift_block(-1)
            return
        if key == Qt.Key.Key_Backspace and self._delete_pair():
            return

        typed = event.text()
        if typed and not control and not alt:
            if typed in _AUTO_PAIRS and self._auto_close(typed):
                return
            if typed in ")]}" and self._skip_closer(typed):
                return

        super().keyPressEvent(event)

    def _insert_newline(self) -> None:
        cursor = self.textCursor()
        line = cursor.block().text()
        indent = line[: len(line) - len(line.lstrip())]
        if line.rstrip()[-1:] in _OPENS_A_BLOCK:
            indent += " " * INDENT
        cursor.insertText("\n" + indent)
        self.refresh_highlights()

    def _handle_tab(self) -> None:
        cursor = self.textCursor()
        if cursor.hasSelection() and self._spans_lines(cursor):
            self._shift_block(+1)
            return
        column = cursor.positionInBlock()
        cursor.insertText(" " * (INDENT - column % INDENT))

    def _auto_close(self, typed: str) -> bool:
        cursor = self.textCursor()
        closer = _AUTO_PAIRS[typed]

        if cursor.hasSelection():
            selected = cursor.selectedText().replace("\u2029", "\n")
            cursor.insertText(typed + selected + closer)
            return True

        if typed in _QUOTES:
            before = self._char_before(cursor)
            # `don't` is not an opening quote, so a quote after a word is just a quote.
            if before and (before.isalnum() or before == "_"):
                return False

        if self._char_at(cursor) == closer:
            cursor.movePosition(QTextCursor.MoveOperation.NextCharacter)
            self.setTextCursor(cursor)
            return True

        cursor.insertText(typed + closer)
        cursor.movePosition(QTextCursor.MoveOperation.PreviousCharacter)
        self.setTextCursor(cursor)
        return True

    def _skip_closer(self, typed: str) -> bool:
        """Typing the partner of a bracket that closed itself steps over it instead of doubling."""
        cursor = self.textCursor()
        if cursor.hasSelection() or self._char_at(cursor) != typed:
            return False
        cursor.movePosition(QTextCursor.MoveOperation.NextCharacter)
        self.setTextCursor(cursor)
        return True

    def _delete_pair(self) -> bool:
        cursor = self.textCursor()
        if cursor.hasSelection():
            return False
        before = self._char_before(cursor)
        after = self._char_at(cursor)
        if before is None or after is None or _AUTO_PAIRS.get(before) != after:
            return False
        cursor.beginEditBlock()
        cursor.deletePreviousChar()
        cursor.deleteChar()
        cursor.endEditBlock()
        self.refresh_highlights()
        return True

    @staticmethod
    def _char_before(cursor: QTextCursor) -> str | None:
        if cursor.position() == 0:
            return None
        probe = QTextCursor(cursor)
        probe.movePosition(QTextCursor.MoveOperation.PreviousCharacter, QTextCursor.MoveMode.KeepAnchor)
        return probe.selectedText() or None

    @staticmethod
    def _char_at(cursor: QTextCursor) -> str | None:
        probe = QTextCursor(cursor)
        probe.movePosition(QTextCursor.MoveOperation.NextCharacter, QTextCursor.MoveMode.KeepAnchor)
        return probe.selectedText() or None

    # -- line surgery ----------------------------------------------------------------------

    def toggle_comment(self) -> None:
        token = _COMMENT.get(self.language, "#")
        first, last = self._selected_blocks()
        lines = self._block_texts(first, last)
        body = [line for line in lines if line.strip()]
        commented = bool(body) and all(line.lstrip().startswith(token) for line in body)
        # One column for the whole run, the shallowest line's, so a block keeps its shape instead
        # of every line being commented where it happens to sit.
        base = min((len(line) - len(line.lstrip()) for line in body), default=0)
        self._rewrite(first, last, [_uncomment(line, token) if commented
                                    else _comment(line, base, token) for line in lines])
        self.refresh_highlights()

    def duplicate_lines(self) -> None:
        first, last = self._selected_blocks()
        lines = self._block_texts(first, last)
        start, span_end, cursor = self._rewrite(first, last, lines + lines)
        # Leave the second copy selected, which is the one you would go on to edit. Where it starts
        # is measured, not halved: the first copy carries a separator after its last line too.
        split = start + sum(len(line) for line in lines) + len(lines)
        cursor.setPosition(split)
        cursor.setPosition(span_end, QTextCursor.MoveMode.KeepAnchor)
        self.setTextCursor(cursor)
        self.refresh_highlights()

    def move_lines(self, delta: int) -> None:
        first, last = self._selected_blocks()
        selected = self._block_texts(first, last)
        if delta < 0:
            other = first.previous()
            if not other.isValid():
                return
            lines = selected + [other.text()]     # the selection goes above its neighbour
            start, end = other.position(), self._region_end(last)
            moved = start
        else:
            other = last.next()
            if not other.isValid():
                return
            if not other.text() and not other.next().isValid():
                # The blank block a trailing newline leaves is the end of the file, not a line to
                # swap into. Without this, moving the last line down just makes a gap above it.
                return
            lines = [other.text()] + selected     # the neighbour goes above the selection
            start, end = first.position(), self._region_end(other)
            moved = start + len(other.text()) + 1

        cursor = self.textCursor()
        cursor.setPosition(start)
        cursor.setPosition(end, QTextCursor.MoveMode.KeepAnchor)
        cursor.beginEditBlock()
        cursor.insertText("\n".join(lines))
        cursor.endEditBlock()

        span_end = (start + sum(len(line) for line in selected) + len(selected) - 1
                    if delta < 0 else end)
        cursor.setPosition(moved)
        cursor.setPosition(span_end, QTextCursor.MoveMode.KeepAnchor)
        self.setTextCursor(cursor)
        self.refresh_highlights()

    def _shift_block(self, delta: int) -> None:
        first, last = self._selected_blocks()
        lines = self._block_texts(first, last)
        shifted = [" " * INDENT + line if line.strip() else line for line in lines] \
            if delta > 0 else [_dedent(line) for line in lines]
        self._rewrite(first, last, shifted)
        self.refresh_highlights()

    def _selected_blocks(self):
        cursor = self.textCursor()
        return (self.document().findBlock(cursor.selectionStart()),
                self.document().findBlock(cursor.selectionEnd()))

    @staticmethod
    def _block_texts(first, last) -> list[str]:
        lines = []
        block = first
        while True:
            lines.append(block.text())
            if block == last:
                return lines
            block = block.next()

    def _spans_lines(self, cursor: QTextCursor) -> bool:
        return self.document().findBlock(cursor.selectionStart()) != \
            self.document().findBlock(cursor.selectionEnd())

    def _region_end(self, block) -> int:
        """The character position just past `block`'s text, separator included."""
        after = block.next()
        return after.position() - 1 if after.isValid() else self.document().characterCount() - 1

    def _rewrite(self, first, last, lines: list[str]):
        """Replace the whole block range with `lines`, and hand back where it landed."""
        start = first.position()
        end = self._region_end(last)

        cursor = self.textCursor()
        cursor.setPosition(start)
        cursor.setPosition(end, QTextCursor.MoveMode.KeepAnchor)
        cursor.beginEditBlock()
        cursor.insertText("\n".join(lines))
        cursor.endEditBlock()

        span_end = start + sum(len(line) for line in lines) + len(lines) - 1
        cursor.setPosition(start)
        cursor.setPosition(span_end, QTextCursor.MoveMode.KeepAnchor)
        self.setTextCursor(cursor)
        return start, span_end, cursor


def _comment(line: str, base: int, token: str) -> str:
    if len(line) <= base or not line.strip():
        return line
    return line[:base] + token + " " + line[base:]


def _uncomment(line: str, token: str) -> str:
    indent = line[: len(line) - len(line.lstrip())]
    body = line[len(indent):]
    if not body.startswith(token):
        return line
    body = body[len(token):]
    if body.startswith(" "):
        body = body[1:]
    return indent + body


def _dedent(line: str) -> str:
    remove = 0
    while remove < INDENT and remove < len(line) and line[remove] == " ":
        remove += 1
    return line[remove:]
