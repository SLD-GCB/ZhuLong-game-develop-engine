"""音符格子：一首曲子的时间线（横轴是拍，纵轴是音高，一个音符一个方块）。

**曲子那份模型不在这一头。** 真正的 `SongSpec` 在宿主里（C++ 的 `song.h`）—— 这个 widget 只做两件
事：**把宿主发下来的音符画出来**，和**把鼠标动作发回去**。发出去的是 `command(名字, 一串数)`，面板
把它转成一行 `V <名字> <数…>` 写下去；宿主编完再推一份新的回来，这里重画。

所以这里没有"曲子的真相"，只有画和命中测试 —— 两条路各干各的，模型只有一份。

几条固定的取舍（写在这儿，因为它们是手感的一部分）：

* **吸附 1/4 拍**（十六分音符），**不能关** —— 点击那条线（`viewport.py` 的 `C`）只带键和位置，
  没有修饰键，所以做不了"按 Shift 不吸附"。
* **横向从按下那一刻算起**（delta），所以抓着音符中间拖它不会跳；**纵向看指针在哪一行**（绝对），
  因为一格就是一个音高，"拖到哪一行"就是那个意思。
* **右缘 6 像素以内算"改长短"**，其余地方是"挪"。
* **一次拖一步撤销**：宿主那边每个 `note_move` 都压一次栈（和建模器拖动一个规矩）。
"""

from __future__ import annotations

from PySide6.QtCore import QPoint, QRect, Qt, Signal
from PySide6.QtGui import QColor, QFont, QPainter
from PySide6.QtWidgets import QSizePolicy, QWidget

# 布局。
_HEADER = 18      # 顶上那条"速度 / 一遍 / 音符数"
_LABEL = 34       # 左边留给音高名的那一溜
_ROW_MIN = 8      # 一行最矮 / 最高（音符多的时候行会变矮）
_ROW_MAX = 26
_EDGE = 6         # 右缘这么多像素以内算"改长短"
_SNAP = 0.25      # 吸附到 1/4 拍

# 一件乐器一个颜色，按乐器号循环。
_COLOURS = (
    QColor("#4ec9b0"), QColor("#d7a65b"), QColor("#c586c0"),
    QColor("#6a9955"), QColor("#569cd6"), QColor("#e06c75"),
)


class TimelineView(QWidget):
    """一首曲子的音符格子。见模块开头那段。"""

    #: 一次编辑：名字，和一串数。面板转成 `V <名字> <数…>`。
    command = Signal(str, list)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._tempo = 120.0
        self._loop = 4.0
        self._instruments = 1
        self._notes: list[tuple[float, float, int, float, int]] = []
        self._selected = -1
        self._play = -1.0     # 播放头在哪一拍；负数 = 不画
        self._drag: tuple | None = None
        self.setMinimumHeight(110)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setToolTip("点空白加一个音符（1 拍长）· 拖着它挪 · 拉右缘改长短 · 右键删掉 · "
                        "Delete 删选中的")

    # -- 数据（宿主推过来的那一份）---------------------------------------------------------

    def clear(self) -> None:
        self._notes = []
        self._selected = -1
        self._play = -1.0
        self._drag = None
        self.update()

    def set_song(self, tempo: float, loop: float, instruments: int,
                 notes: list[tuple[float, float, int, float, int]]) -> None:
        self._tempo = tempo
        self._loop = max(float(loop), _SNAP)
        self._instruments = max(int(instruments), 1)
        self._notes = notes
        if self._selected >= len(self._notes):
            self._selected = -1
        self.update()

    def set_playhead(self, beats: float) -> None:
        self._play = beats
        self.update()

    # -- 几何 ----------------------------------------------------------------------------

    def _pitch_range(self) -> tuple[int, int]:
        """音高范围：按音符自适应，两头各留 6 个半音，至少 24 行。"""
        if self._notes:
            low = min(note[2] for note in self._notes) - 6
            high = max(note[2] for note in self._notes) + 6
        else:
            low, high = 54, 78
        while high - low < 23:
            low -= 1
            high += 1
        return max(0, low), min(127, high)

    def _plot(self) -> QRect:
        return QRect(_LABEL, _HEADER, max(1, self.width() - _LABEL - 1),
                     max(1, self.height() - _HEADER - 1))

    def _row_h(self) -> float:
        low, high = self._pitch_range()
        return max(_ROW_MIN, min(_ROW_MAX, self._plot().height() / (high - low + 1)))

    def _x_of(self, beat: float) -> float:
        plot = self._plot()
        return plot.x() + beat / self._loop * plot.width()

    def _beat_at(self, x: float) -> float:
        plot = self._plot()
        return (x - plot.x()) / max(1, plot.width()) * self._loop

    def _pitch_at(self, y: float) -> int:
        low, high = self._pitch_range()
        row = int((self._plot().bottom() - y) / self._row_h())
        return max(low, min(high, low + row))

    def _rect_of(self, note: tuple) -> QRect:
        start, beats, pitch, _velocity, _instrument = note
        low, _high = self._pitch_range()
        row = self._row_h()
        x0 = self._x_of(start)
        x1 = self._x_of(start + beats)
        y0 = self._plot().bottom() - (pitch - low + 1) * row
        return QRect(int(x0), int(y0), max(3, int(x1 - x0)), max(2, int(row)))

    def _note_at(self, pos: QPoint) -> int:
        # 从后往前：后加的画在上面，也先被点到。
        for index in range(len(self._notes) - 1, -1, -1):
            if self._rect_of(self._notes[index]).contains(pos):
                return index
        return -1

    # -- 画 ------------------------------------------------------------------------------

    def paintEvent(self, _event) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#141414"))
        low, high = self._pitch_range()
        plot = self._plot()
        row = self._row_h()

        # 音高行：C 那一行铺一层亮一点的底，行与行之间一条暗线，左边写上音名。
        painter.setFont(QFont("Consolas", 7))
        for pitch in range(low, high + 1):
            y0 = int(plot.bottom() - (pitch - low + 1) * row)
            if pitch % 12 == 0:
                painter.fillRect(QRect(plot.x(), y0, plot.width(), max(1, int(row))),
                                 QColor("#232323"))
                painter.setPen(QColor("#7a8494"))
                painter.drawText(QRect(2, y0, _LABEL - 6, max(8, int(row))),
                                 Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight,
                                 f"C{pitch // 12 - 1}")
            painter.setPen(QColor("#1e1e1e"))
            painter.drawLine(plot.x(), y0, plot.right(), y0)

        # 拍线：每 4 拍一条亮一点的（小节线）。
        for beat in range(0, int(self._loop) + 1):
            x = int(self._x_of(beat))
            painter.setPen(QColor("#5a5a5a") if beat % 4 == 0 else QColor("#333333"))
            painter.drawLine(x, plot.y(), x, plot.bottom())

        # 音符。
        painter.setFont(QFont("Consolas", 7))
        for index, note in enumerate(self._notes):
            rect = self._rect_of(note)
            colour = _COLOURS[note[4] % len(_COLOURS)]
            selected = index == self._selected
            painter.fillRect(rect, colour if selected else colour.darker(150))
            painter.setPen(QColor("#ffffff") if selected else QColor("#0d0d0d"))
            painter.drawRect(rect)
            if rect.width() > 24:
                painter.setPen(QColor("#f0f0f0") if selected else QColor("#111111"))
                painter.drawText(rect.adjusted(3, 0, -2, 0), Qt.AlignmentFlag.AlignVCenter,
                                 str(note[2]))

        # 播放头。
        if self._play >= 0.0:
            x = int(self._x_of(self._play % self._loop))
            painter.setPen(QColor("#e0c060"))
            painter.drawLine(x, plot.y(), x, plot.bottom())

        # 顶上一行字。
        painter.setPen(QColor("#9da5b4"))
        painter.setFont(QFont("Microsoft YaHei", 8))
        summary = (f"速度 {self._tempo:g} BPM　·　一遍 {self._loop:g} 拍　·　"
                   f"{len(self._notes)} 个音符　·　吸附 1/4 拍")
        if not self._notes:
            summary += "　·　点空格子加一个音符"
        painter.drawText(QRect(6, 0, self.width() - 12, _HEADER),
                         Qt.AlignmentFlag.AlignVCenter, summary)

    # -- 鼠标 / 键盘 ---------------------------------------------------------------------

    def mousePressEvent(self, event) -> None:  # noqa: N802 - Qt's name
        pos = event.position().toPoint()
        if event.button() == Qt.MouseButton.RightButton:
            index = self._note_at(pos)
            if index >= 0:
                self.command.emit("note_del", [float(index)])
                self._selected = -1
                self.update()
            return
        if event.button() != Qt.MouseButton.LeftButton:
            return
        self.setFocus(Qt.FocusReason.MouseFocusReason)

        index = self._note_at(pos)
        if index < 0:
            # 空白：**加一个音符**（1 拍长），落在吸附后的拍和那一行上。
            beat = max(0.0, min(self._loop - _SNAP, round(self._beat_at(pos.x()) / _SNAP) * _SNAP))
            pitch = self._pitch_at(pos.y())
            self.command.emit("note_add", [beat, 1.0, float(pitch), 0.9, 0.0])
            # 新的是最后一个（宿主按追加处理），先把它记成选中的 —— 表推回来就对上了。
            self._selected = len(self._notes)
            self._drag = None
            self.update()
            return

        self._selected = index
        rect = self._rect_of(self._notes[index])
        mode = "len" if pos.x() >= rect.right() - _EDGE else "move"
        self._drag = (mode, index, pos.x(), self._notes[index])
        self.update()

    def mouseMoveEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if self._drag is None:
            return
        mode, index, from_x, origin = self._drag
        if index >= len(self._notes):
            return
        pos = event.position().toPoint()
        start, beats, pitch, velocity, instrument = origin
        plot = self._plot()

        if mode == "move":
            delta = (pos.x() - from_x) / max(1, plot.width()) * self._loop
            new_start = max(0.0, min(self._loop - _SNAP, round((start + delta) / _SNAP) * _SNAP))
            new_pitch = self._pitch_at(pos.y())
            if abs(new_start - self._notes[index][0]) > 1e-6 or \
                    new_pitch != self._notes[index][2]:
                self._notes[index] = (new_start, beats, new_pitch, velocity, instrument)
                self.command.emit("note_move", [float(index), new_start, float(new_pitch)])
                self.update()
        else:
            beat = self._beat_at(pos.x())
            new_beats = max(_SNAP, round((beat - start) / _SNAP) * _SNAP)
            if abs(new_beats - self._notes[index][1]) > 1e-6:
                self._notes[index] = (start, new_beats, pitch, velocity, instrument)
                self.command.emit("note_len", [float(index), new_beats])
                self.update()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag = None

    def keyPressEvent(self, event) -> None:  # noqa: N802 - Qt's name
        if event.key() in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace):
            if 0 <= self._selected < len(self._notes):
                self.command.emit("note_del", [float(self._selected)])
                self._selected = -1
                self.update()
            event.accept()
            return
        super().keyPressEvent(event)
