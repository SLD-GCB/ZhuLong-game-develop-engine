"""乐器的图形编辑器：一首曲子里那几件乐器（波形 + ADSR + 增益 + 低通）。

和「音效…」那个对话框是**同一族**的东西，区别只有一个：**乐器没有音高和长度** —— 那两样由音符给
（`song.h` 里 `Instrument` 那一段就是为这个才砍掉 `f0/f1/seconds` 的）。左边是乐器列表，右边是它那
几个参数。

**改一下就发**，没有"确定/取消"，只有「关闭」—— 设计音色的手感就是"调一个数听一下"。初值来自宿主
推过来的 `instrument` 行（不是这里猜的），而且**用户正在编的那个框不回填**，免得一边打字一边被顶掉
（和速度/一遍那两个框一个规矩）。

发出去的是 `command(名字, 一串数)`，面板把它转成一行 `V <名字> <数…>` 写下去。
"""

from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

# 波形那张小表。**正本是 `synth.h` 的 `SynthSpec::Wave`** —— Python 这一头留一份是因为跑不过去，
# 场景文本那边留一份是因为它是格式。三处，改一处要改三处。
_WAVES = ("正弦", "方波", "锯齿", "三角", "噪声")

# 音名，也是为了在界面上说人话（`60` → `C4`）。**正本是 `song.h` 的 `PitchHz` 那套十二平均律。**
_NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


def _note_name(pitch: int) -> str:
    return f"{_NOTE_NAMES[pitch % 12]}{pitch // 12 - 1}"

# 那六个框：名字、标签、最小、最大、步长、小数位。
_FIELDS = (
    ("attack", "起音 秒", 0.0, 5.0, 0.005, 3),
    ("decay", "衰减 秒", 0.0, 5.0, 0.005, 3),
    ("sustain", "持续电平 0..1", 0.0, 1.0, 0.05, 2),
    ("release", "释音 秒", 0.0, 5.0, 0.005, 3),
    ("gain", "增益", 0.0, 2.0, 0.05, 2),
    ("cutoff", "低通 Hz（0 = 关）", 0.0, 20000.0, 100.0, 0),
)


class InstrumentDialog(QDialog):
    """一首曲子的乐器编辑器。见模块开头那段。"""

    #: 一次编辑：名字，和一串数。面板转成 `V <名字> <数…>`。
    command = Signal(str, list)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("乐器")
        self.resize(560, 340)
        # 一件乐器 = (波形, 起音, 衰减, 持续, 释音, 增益, 低通, 种子)。种子只有噪声用得上，
        # 这里不给编辑它的地方，但**得原样带回去**，不然一改别的就把种子抹了。
        self._instruments: list[tuple] = []
        self._index = -1
        self._playing = -1     # 正在试听第几件；-1 = 没在试听
        self._syncing = False  # 正在回填 UI（别把回填当成用户改的）

        layout = QVBoxLayout(self)
        body = QHBoxLayout()

        self._list = QListWidget()
        self._list.setFixedWidth(150)
        self._list.currentRowChanged.connect(self._on_row)
        body.addWidget(self._list)

        form = QFormLayout()
        self._wave = QComboBox()
        for label in _WAVES:
            self._wave.addItem(label)
        self._wave.currentIndexChanged.connect(self._send)
        form.addRow("波形", self._wave)
        self._boxes: dict[str, QDoubleSpinBox] = {}
        for key, label, low, high, step, decimals in _FIELDS:
            box = QDoubleSpinBox()
            box.setRange(low, high)
            box.setSingleStep(step)
            box.setDecimals(decimals)
            box.valueChanged.connect(self._send)
            form.addRow(label, box)
            self._boxes[key] = box
        body.addLayout(form, 1)
        layout.addLayout(body, 1)

        buttons = QHBoxLayout()
        self._add = QPushButton("新增乐器")
        self._add.setToolTip("追加一件默认的（正弦、中庸包络）")
        self._add.clicked.connect(lambda: self.command.emit("instrument_add", []))
        buttons.addWidget(self._add)
        self._delete = QPushButton("删除乐器")
        self._delete.setToolTip("删掉选中的那件。还有音符在用、或者只剩一件的话，程序会拒绝并说清为什么")
        self._delete.clicked.connect(self._delete_current)
        buttons.addWidget(self._delete)
        self._audition = QPushButton("试听")
        self._audition.setToolTip("拿这件乐器循环响这一串音（带节奏），边调边听。"
                                  "下面那两个框：写音序、和没写长度时用多长")
        self._audition.clicked.connect(self._toggle_audition)
        buttons.addWidget(self._audition)
        buttons.addStretch(1)
        close = QPushButton("关闭")
        close.clicked.connect(self.accept)
        buttons.addWidget(close)
        layout.addLayout(buttons)

        # **试听的音序（带节奏）和每音的音长**。一步写成 `音高`、`音高:秒数`（这一个音自己的长度）
        # 或者 `-:秒数`（休止）—— 有了 `:秒数` 就有节奏了（长音短音、该停的地方停）。
        controls = QHBoxLayout()
        controls.addWidget(QLabel("试听音序"))
        self._notes = QLineEdit("60:1 64:0.5 67:0.5 72:1")
        self._notes.setToolTip("试听这一串：0..127 的 MIDI 号。`音高:秒数` 给这一个音自己的长度，"
                              "`-` 是休止 —— 比如 `60:1 64 64 -:0.5 67:0.5`")
        self._notes.setMinimumWidth(200)
        self._notes.setMaximumWidth(260)
        self._notes.textChanged.connect(self._on_notes)
        controls.addWidget(self._notes)
        self._names = QLabel("")
        controls.addWidget(self._names)
        controls.addWidget(QLabel("默认"))
        self._length = QDoubleSpinBox()
        self._length.setRange(0.05, 8.0)
        self._length.setDecimals(2)
        self._length.setSingleStep(0.05)
        self._length.setValue(0.30)
        self._length.setSuffix(" 秒")
        self._length.setToolTip("没写 `:秒数` 的那些音用这个长度 —— 改它那一个会从头来一遍")
        self._length.valueChanged.connect(self._on_length)
        controls.addWidget(self._length)
        controls.addStretch(1)
        layout.addLayout(controls)
        self._on_notes()   # 把音名那条先填上

        self._hint = QLabel("")
        layout.addWidget(self._hint)

    # -- 数据（宿主推过来的那一份）---------------------------------------------------------

    def set_instruments(self, instruments: list[tuple]) -> None:
        """重建列表，并把当前那一件填进表单。**不重建正在编辑的那些框**（`_apply` 里让着焦点）。"""
        self._instruments = instruments
        if self._playing >= len(instruments):
            self._playing = -1
            self._audition.setText("试听")
        self._syncing = True
        self._list.clear()
        for index, instrument in enumerate(instruments):
            self._list.addItem(f"{index + 1}  {_WAVES[instrument[0] % len(_WAVES)]}")
        if self._index < 0 or self._index >= len(instruments):
            self._index = 0 if instruments else -1
        self._list.setCurrentRow(self._index if instruments else -1)
        self._syncing = False
        self._apply(self._index)

    def stop_audition(self) -> None:
        """关掉对话框之前叫一声 —— 试听是**循环**的，不能让它一直响下去。"""
        if self._playing >= 0:
            self.command.emit("instrument_stop", [])
            self._playing = -1
            self._audition.setText("试听")

    # -- 内部 ----------------------------------------------------------------------------

    def _on_row(self, row: int) -> None:
        if self._syncing:
            return
        self._index = row
        self._apply(row)
        # 正在试听的时候换一件：**把试听也挪过去** —— 不然会一边编着 B、一边听着 A 响。
        if self._playing >= 0 and 0 <= row < len(self._instruments):
            args = self._audition_args(row)
            if args is not None:
                self.command.emit("instrument_play", args)
                self._playing = row

    def _parse_steps(self) -> list[tuple[int, float]] | None:
        """把音序那个框读成一串「一步」：`(音高, 秒)`，音高 -1 = 休止。

        一步可以写成 `60`（用「默认」那个长度）、`60:0.5`（这一个音自己的长度）、或者 `-:0.5`
        （休止这么久）。读不出来（不是整数、越界、秒数读不出来）就是 None。
        """
        text = self._notes.text().replace("，", " ").replace(",", " ")
        default = self._length.value()
        steps: list[tuple[int, float]] = []
        for part in text.split():
            head, colon, tail = part.partition(":")
            if head in ("-", "r", "R", "休"):
                pitch = -1
            else:
                try:
                    pitch = int(head)
                except ValueError:
                    return None
                if not 0 <= pitch <= 127:
                    return None
            seconds = default
            if colon:
                try:
                    seconds = float(tail)
                except ValueError:
                    return None
                if not 0.05 <= seconds <= 8.0:
                    return None
            steps.append((pitch, seconds))
        return steps[:32]

    def _audition_args(self, index: int) -> list | None:
        """试听那一行的参数：哪一件，后面跟着一**对**一对的（音高, 秒）。读不出音序就是 None。"""
        steps = self._parse_steps()
        if steps is None:
            return None
        args: list[float] = [float(index)]
        for pitch, seconds in steps:
            args.append(float(pitch))
            args.append(seconds)
        return args

    def _on_notes(self) -> None:
        """改了音序：刷新音名那条；正在响、而且读得出来，就重来一遍。

        **换音序会让那一个从头来**（一串别的音就是新的一份样本）—— 和改参数、改音长一样，
        不像以前那个"转音高不停声"。
        """
        steps = self._parse_steps()
        if steps is None:
            self._names.setText("认不出（音高 0..127；`音高:秒数` 给长度，`-` 是休止）")
            return
        shown = " ".join("·" if pitch < 0 else _note_name(pitch) for pitch, _ in steps[:16])
        total = sum(seconds for _, seconds in steps)
        self._names.setText(f"{shown}{' …' if len(steps) > 16 else ''}　共 {total:.2f} 秒")
        if self._playing >= 0:
            args = self._audition_args(self._playing)
            if args is not None:
                self.command.emit("instrument_play", args)

    def _on_length(self, _value: float) -> None:
        """改了默认音长。正在响的话重来一遍 —— **换音长就是换一段样本**（帧数变了，指纹变了）。"""
        if self._playing >= 0:
            args = self._audition_args(self._playing)
            if args is not None:
                self.command.emit("instrument_play", args)

    def _apply(self, row: int) -> None:
        """把表单填成第 row 件乐器。"""
        if not (0 <= row < len(self._instruments)):
            return
        wave, attack, decay, sustain, release, gain, cutoff, _seed = self._instruments[row]
        values = {"attack": attack, "decay": decay, "sustain": sustain, "release": release,
                  "gain": gain, "cutoff": cutoff}
        self._syncing = True
        self._wave.setCurrentIndex(max(0, min(int(wave), len(_WAVES) - 1)))
        for key, box in self._boxes.items():
            if not box.hasFocus():
                box.setValue(values[key])
        self._syncing = False
        self._delete.setEnabled(len(self._instruments) > 1)
        self._hint.setText(f"共 {len(self._instruments)} 件乐器")

    def _send(self, *_ignored) -> None:
        """改了一个数：**整件发一遍**（原子，不用逐字段对账）。"""
        if self._syncing or not (0 <= self._index < len(self._instruments)):
            return
        seed = self._instruments[self._index][7]
        self.command.emit("instrument", [
            float(self._index),
            float(self._wave.currentIndex()),
            self._boxes["attack"].value(),
            self._boxes["decay"].value(),
            self._boxes["sustain"].value(),
            self._boxes["release"].value(),
            self._boxes["gain"].value(),
            self._boxes["cutoff"].value(),
            float(seed),
        ])

    def _delete_current(self) -> None:
        if 0 <= self._index < len(self._instruments):
            self.command.emit("instrument_del", [float(self._index)])

    def _toggle_audition(self) -> None:
        if self._playing >= 0:
            self.command.emit("instrument_stop", [])
            self._playing = -1
            self._audition.setText("试听")
            return
        if 0 <= self._index < len(self._instruments):
            args = self._audition_args(self._index)
            if args is None:
                self._hint.setText("音序读不出来 —— 要一串 0..127 的 MIDI 号")
                return
            self.command.emit("instrument_play", args)
            self._playing = self._index
            self._audition.setText("停止试听")

    def closeEvent(self, event) -> None:  # noqa: N802 - Qt's name
        self.stop_audition()
        super().closeEvent(event)

    def accept(self) -> None:
        self.stop_audition()
        super().accept()

    def reject(self) -> None:
        # **每一条关掉的路都要停下试听** —— 它是循环的，不管怎么关都不能让它一直响下去。面板在
        # "选中的不再是曲子"时走的就是这一条（`reject`）。
        self.stop_audition()
        super().reject()
