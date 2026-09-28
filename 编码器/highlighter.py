"""Syntax highlighting, which is what makes the editor an editor.

Kept to one regex per rule and no state, because the point is that a keyword looks like a keyword,
not that a parser is being written. Comments and multiline strings are the things that run across
lines, and they are handled by the block rule at the bottom.

Two languages: Python, which drives the machine through the module, and C++, which is what the
machine itself is made of.
"""

from __future__ import annotations

import keyword

from PySide6.QtCore import QRegularExpression
from PySide6.QtGui import QColor, QFont, QSyntaxHighlighter, QTextCharFormat

# States carried from one block to the next, so a construct that runs across lines keeps its colour.
_NONE = 0
_PY_SINGLE = 1
_PY_DOUBLE = 2
_C_BLOCK = 3


def _format(colour: str, bold: bool = False, italic: bool = False) -> QTextCharFormat:
    made = QTextCharFormat()
    made.setForeground(QColor(colour))
    if bold:
        made.setFontWeight(QFont.Weight.Bold)
    made.setFontItalic(italic)
    return made


class PythonHighlighter(QSyntaxHighlighter):
    def __init__(self, document) -> None:
        super().__init__(document)

        self._rules: list[tuple[QRegularExpression, QTextCharFormat]] = []

        # Keywords, taken from the interpreter rather than typed out.
        keyword_format = _format("#c586c0", bold=True)
        for word in keyword.kwlist:
            self._rules.append(
                (QRegularExpression(rf"\b{word}\b"), keyword_format)
            )

        # Three of these read as builtins rather than keywords, and they are the three a scene
        # script actually calls.
        builtin_format = _format("#4ec9b0")
        for word in ("self", "None", "True", "False", "print", "range", "len", "__init__"):
            self._rules.append((QRegularExpression(rf"\b{word}\b"), builtin_format))

        self._rules.append((QRegularExpression(r"\b[0-9]+\.?[0-9]*\b"), _format("#b5cea8")))
        self._rules.append((QRegularExpression(r'"[^"\\]*(\\.[^"\\]*)*"'), _format("#ce9178")))
        self._rules.append((QRegularExpression(r"'[^'\\]*(\\.[^'\\]*)*'"), _format("#ce9178")))
        self._rules.append(
            (
                QRegularExpression(r"#[^\n]*"),
                _format("#6a9955", italic=True),
            )
        )
        self._rules.append((QRegularExpression(r"\bdef\s+(\w+)"), _format("#dcdcaa")))
        self._rules.append((QRegularExpression(r"\bclass\s+(\w+)"), _format("#4ec9b0", bold=True)))

        # Triple-quoted strings are the one thing that runs across lines, so they get the block
        # treatment: the highlighter is asked for each line with a state carried over.
        self._triple_single = QRegularExpression("'''")
        self._triple_double = QRegularExpression('"""')
        self._string_format = _format("#ce9178")

    def highlightBlock(self, text: str) -> None:  # noqa: N802 - Qt's name
        for pattern, style in self._rules:
            iterator = pattern.globalMatch(text)
            while iterator.hasNext():
                match = iterator.next()
                self.setFormat(match.capturedStart(), match.capturedLength(), style)

        self._highlight_multiline(text)

    def _highlight_multiline(self, text: str) -> None:
        # States: 0 none, 1 inside ''', 2 inside """.
        self.setCurrentBlockState(_NONE)
        start = 0
        if self.previousBlockState() != _PY_SINGLE:
            match = self._triple_single.match(text)
            start = match.capturedStart() if match.hasMatch() else -1

        while start >= 0:
            end_match = self._triple_single.match(text, start + 3)
            if end_match.hasMatch():
                length = end_match.capturedStart() - start + 3
                self.setFormat(start, length, self._string_format)
                start = self._triple_single.match(text, start + length).capturedStart()
            else:
                self.setCurrentBlockState(_PY_SINGLE)
                self.setFormat(start, len(text) - start, self._string_format)
                break

        # The double-quoted form is handled the same way, in its own pass.
        start = 0
        if self.previousBlockState() != _PY_DOUBLE:
            match = self._triple_double.match(text)
            start = match.capturedStart() if match.hasMatch() else -1

        while start >= 0:
            end_match = self._triple_double.match(text, start + 3)
            if end_match.hasMatch():
                length = end_match.capturedStart() - start + 3
                self.setFormat(start, length, self._string_format)
                start = self._triple_double.match(text, start + length).capturedStart()
            else:
                self.setCurrentBlockState(_PY_DOUBLE)
                self.setFormat(start, len(text) - start, self._string_format)
                break


# The words a C++ file is made of. Types are separated from control words because they read
# differently and a reader's eye wants them to.
_CPP_KEYWORDS = (
    "alignas", "alignof", "auto", "break", "case", "catch", "class", "co_await", "co_return",
    "co_yield", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
    "continue", "decltype", "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit",
    "export", "extern", "final", "for", "friend", "goto", "if", "inline", "mutable", "namespace",
    "new", "noexcept", "operator", "override", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "return", "sizeof", "static", "static_assert", "static_cast",
    "struct", "switch", "template", "this", "thread_local", "throw", "try", "typedef", "typeid",
    "typename", "union", "using", "virtual", "volatile", "while",
)

_CPP_TYPES = (
    "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long", "short",
    "signed", "unsigned", "void", "wchar_t", "size_t", "ptrdiff_t", "int8_t", "int16_t", "int32_t",
    "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "nullptr_t", "true", "false",
    "nullptr", "std", "string", "vector", "array", "span", "optional", "unique_ptr", "shared_ptr",
    "make_unique", "make_shared", "move", "forward", "printf", "fprintf",
)

_CPP_PREPROCESSOR = (
    "include", "define", "undef", "ifdef", "ifndef", "endif", "else", "elif", "pragma", "error",
    "warning", "line",
)


class CppHighlighter(QSyntaxHighlighter):
    """Highlighting for the language the machine is written in.

    Literals and comments are found first and everything else is painted around them, because those
    are the two places a keyword regex must not reach into: `#include "zlong/system/system.h"` is
    not a path full of types, and a `//` inside a string does not start a comment.
    """

    def __init__(self, document) -> None:
        super().__init__(document)

        self._comment_format = _format("#6a9955", italic=True)
        self._string_format = _format("#ce9178")
        self._strings = QRegularExpression(r'"(?:[^"\\]|\\.)*"')
        self._characters = QRegularExpression(r"'(?:[^'\\]|\\.)*'")

        self._rules: list[tuple[QRegularExpression, QTextCharFormat]] = []
        keyword_format = _format("#569cd6", bold=True)
        for word in _CPP_KEYWORDS:
            self._rules.append((QRegularExpression(rf"\b{word}\b"), keyword_format))
        type_format = _format("#4ec9b0")
        for word in _CPP_TYPES:
            self._rules.append((QRegularExpression(rf"\b{word}\b"), type_format))

        # A directive and the header it names read as two different things.
        self._rules.append((QRegularExpression(r"^[ \t]*#[ \t]*[a-z]+"),
                            _format("#c586c0", bold=True)))
        self._rules.append((QRegularExpression(r"#[ \t]*[a-z]+\b"), _format("#c586c0")))
        for word in _CPP_PREPROCESSOR:
            self._rules.append((QRegularExpression(rf"#[ \t]*{word}\b"), _format("#c586c0")))
        self._rules.append((QRegularExpression(r"<[A-Za-z0-9_./]+>"), _format("#ce9178")))

        self._rules.append((QRegularExpression(r"\b[0-9]+\.?[0-9]*(?:[uUlLfF]+)?\b"),
                            _format("#b5cea8")))
        # A capitalised word is a type here by convention -- this project names them that way.
        self._rules.append((QRegularExpression(r"\b[A-Z][A-Za-z0-9_]*\b"), _format("#4ec9b0")))

    def highlightBlock(self, text: str) -> None:  # noqa: N802 - Qt's name
        literals = self._literal_spans(text)
        comments = self._comment_spans(text, literals)
        reserved = literals + comments

        for pattern, style in self._rules:
            iterator = pattern.globalMatch(text)
            while iterator.hasNext():
                match = iterator.next()
                if _inside(reserved, match.capturedStart()):
                    continue
                self.setFormat(match.capturedStart(), match.capturedLength(), style)

        for start, length in literals:
            self.setFormat(start, length, self._string_format)
        for start, length in comments:
            self.setFormat(start, length, self._comment_format)

    def _literal_spans(self, text: str) -> list[tuple[int, int]]:
        spans: list[tuple[int, int]] = []
        for pattern in (self._strings, self._characters):
            iterator = pattern.globalMatch(text)
            while iterator.hasNext():
                match = iterator.next()
                spans.append((match.capturedStart(), match.capturedLength()))
        return spans

    def _comment_spans(self, text: str, literals: list[tuple[int, int]]) -> list[tuple[int, int]]:
        spans: list[tuple[int, int]] = []
        in_block = self.previousBlockState() == _C_BLOCK
        self.setCurrentBlockState(_NONE)

        if in_block:
            end = text.find("*/")
            if end < 0:
                self.setCurrentBlockState(_C_BLOCK)
                return [(0, len(text))]
            spans.append((0, end + 2))
            index = end + 2
        else:
            index = 0

        while index < len(text):
            if _inside(literals, index):
                index += 1
                continue
            if text.startswith("//", index):
                spans.append((index, len(text) - index))
                return spans
            if text.startswith("/*", index):
                end = text.find("*/", index + 2)
                if end < 0:
                    self.setCurrentBlockState(_C_BLOCK)
                    spans.append((index, len(text) - index))
                    return spans
                spans.append((index, end + 2 - index))
                index = end + 2
                continue
            index += 1
        return spans


def _inside(spans: list[tuple[int, int]], position: int) -> bool:
    return any(start <= position < start + length for start, length in spans)
