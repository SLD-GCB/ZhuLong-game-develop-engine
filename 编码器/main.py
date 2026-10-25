"""烛龙 编码器 — the code editor that the rest of the tool is built on.

A game here is not configured through panels; it is *written*. So the first piece of the studio is
the thing you write in. The editor is deliberately plain: a file tree, tabs, and a Run button.

Run means one of two things, because the project has two faces:

* a **Python** file is run by an interpreter that has the built `zlong` module importable;
* a **C++** file is compiled with MSVC and linked against the libraries in `build/`, and the result
  is what runs. The engine is C++, so this is the same machine the Python face talks to, without the
  binding in between.

    python 编码器/main.py [file ...]
"""

from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtCore import QDir, QModelIndex, QSettings, Qt
from PySide6.QtGui import QAction, QActionGroup, QColor, QFont, QIcon, QKeySequence, QPalette
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFileDialog,
    QFileSystemModel,
    QFormLayout,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QMainWindow,
    QMessageBox,
    QColorDialog,
    QPushButton,
    QSplitter,
    QStackedWidget,
    QTabWidget,
    QTreeView,
    QVBoxLayout,
    QWidget,
)

import cpp
import packager
import templates
from console import Console
from editor import CodeEditor
from findbar import FindBar
from instruments import InstrumentDialog
from runtime import SCENE_PREFIX, Command, Runner, editor_asset, find_backend, program_command, \
    project_root, python_command, split_arguments
from timeline import TimelineView
from viewport import Viewport
from window import Mirror

# What each suffix means, so a file opened from the tree is edited and run as what it is.
_LANGUAGES = {
    ".py": "python",
    ".pyw": "python",
    ".cpp": "cpp",
    ".cc": "cpp",
    ".cxx": "cpp",
    ".h": "cpp",
    ".hpp": "cpp",
}

# The dropdown, and the suffix a new file gets in each. Ordered so the list reads as the choice it
# is: the two languages that can be run, and then the one that is only text.
_CHOICES = (
    ("Python", "python", ".py"),
    ("C++", "cpp", ".cpp"),
    ("纯文本", "text", ".txt"),
)

# The marker line that says which half of a Run is going on.
_BUILDING = "编译中…"
_RUNNING = "运行中…"

# 哪几页，按 `QStackedWidget` 的下标排。三页了：代码、3D 建模、音频。
_PAGE_CODE = 0
_PAGE_MODEL = 1
_PAGE_AUDIO = 2

# 建模器在哪儿找，按顺序。**这是两个不同的地方**：
#
#   包里那个        打包时从 Zig 的构建树里搬进来的成品（包里没有源码，也没有编译器能编它）
#   build-zig 那个  源码树里构建出来的那一个
#
# 都找不到就在控制台里说清楚，而不是起一个空的东西。
_MODELER_PATHS = (
    ("建模器", "建模器.exe"),
    ("build-zig", "宿主", "zlong_modeler.exe"),
)

# 音频器在哪儿找，按同样的顺序和同样的道理。CMake 目标叫 `zlong_audiotool`（不是 `zlong_audio`
# —— 那个名字已经是内核那层音频库了）。
_AUDIO_PATHS = (
    ("音频器", "音频器.exe"),
    ("build-zig", "宿主", "zlong_audiotool.exe"),
)

# 建模页上那几个按钮：**它们不自己实现任何东西，只替用户敲一下这个键**。
#
# 键码是 Windows 的虚拟键码，`'1'` `'2'` `'Z'` 各自就是自己的 ASCII。所以这一页和"在编辑器里
# 跑一个程序、用键盘操作它"是同一条路，只是排得顺手一点 —— 协议在 `05-编码器.md` 里。
_MODEL_BUTTONS = (
    ("选面", 49, "选一个面（键 1）"),
    ("选点", 50, "选一个顶点（键 2）"),
    ("选对象", 51, "选中一整个图形/对象（键 3）：点一下就选中它，按住左键整块拖"),
    ("挤出", 69, "把选中的面沿它的法线挤出一层（键 E）"),
    ("细分", 68, "Catmull-Clark 细分一次，整个模型（键 D）"),
    ("切角", 66, "把选中的角切掉（键 B）：点模式切那个点，面模式切那个面的四个角"),
    ("硬边", 67, "把选中那个面的一圈边标成硬边 / 取消（键 C），细分时它们不跟着变圆"),
    ("删除", 88, "删掉选中的东西（键 X）：对象模式删整个对象，面模式删那个面（在那儿留一个洞）"),
    ("旋转", 82, "旋转选中的对象（键 R）：点它之后**左右移鼠标绕 Y 转、上下移绕 X 转**，左键定下来，Esc 取消"),
    ("缩放", 84, "缩放选中的对象（键 T）：点它之后**左右移鼠标**改大小，左键定下来，Esc 取消"),
    ("复制", 86, "复制当前对象（键 V）：新的一份沿画面往右错开一点，直接就是选中的"),
    ("合并", 74, "把选中的多个对象合并成一个（键 J）：先在对象模式下 Shift 点加选，再点这里"),
    ("落下", 70, "把选中的对象落下去（键 F）：落到地面上，或者叠到它正下方那个对象的顶上"),
    ("焊接", 87, "把贴在一起的两块焊成一张连续的面（键 W）：先选中它们，再点这里"),
    ("并集", 85, "布尔并集（键 U）：选中两块，合成一块（去掉里面重叠的部分）"),
    ("交集", 73, "布尔交集（键 I）：只留两块重叠的部分"),
    ("差集", 75, "布尔差集（键 K）：从主对象里挖掉另一个（主 = 最后点的那一个）"),
    ("撤销", 90, "撤销上一步（键 Z）"),
    ("坐标轴", 89, "三条坐标轴开 / 关（键 Y）—— 默认不显示"),
)

# 图元那一排按钮：**它们发的是一个号，不是一次按键。** 形状是个"值" —— 和「上色」「贴图」一样，
# 敲一下键带不走它。所以走的是 `P <图元号>`，程序那头收到就把这个形状拿在手里，下一次左键放在画面上。
#
# 号是两边约好的（`宿主/modeler/main.cpp` 的 `Primitive` 枚举，`宿主/editor_host.h` 的 `P` 说明）。
# 它们是**一次性**的：放一个就回到选择；要再放一个，再点一下这里。
_MODEL_PRIMITIVES = (
    ("方块", 1, "放一个立方体（单位大小）"),
    ("网格", 2, "放一块平面网格"),
    ("圆盘", 3, "放一个圆盘"),
    ("圆柱", 4, "放一个圆柱"),
    ("圆锥", 5, "放一个圆锥"),
    ("球", 6, "放一个球"),
    ("圆环", 7, "放一个圆环"),
    ("金字塔", 8, "放一个金字塔（方底 + 尖）"),
    ("圆台", 9, "放一个圆台（截了顶的圆锥）"),
)

# 工具那块画面出多少像素。**形状跟着面板走，份量保持在这儿** —— 见 `_render_size`（建模器和
# 音频器共用这一份）。
#
# 800×500 就是建模器自己的默认值，也是量着帧率挑出来的那一档（`宿主/modeler` 开头那张表：800×500
# 是 54 帧，1024×640 掉到 34 帧）。所以这里只换形状、不涨份量：图铺满面板，帧率一动不动。
# 想要更清楚，把这个数乘个系数就行，代价写在那一张表里。
_MODEL_PIXELS = 800 * 500

# 音频页上那几个按钮：**它们不自己实现任何东西，只替用户敲一下这个键**。和建模页同一套 —— 键码
# 是 Windows 的虚拟键码，空格 `' '` 是 32、`L` 76、`M` 77、`X` 88、`Z` 90、上下箭头 38 / 40。
_AUDIO_BUTTONS = (
    ("播放/暂停", 32, "空格：开始 / 停下来（声音是每帧现混进声卡的）"),
    ("循环", 76, "把选中声源的循环开关翻一下（键 L）"),
    ("静音", 77, "把选中声源静音 / 取消静音（键 M）"),
    ("删除", 88, "删掉选中的声源（键 X）"),
    ("上一个", 38, "选中上一个声源（↑）"),
    ("下一个", 40, "选中下一个声源（↓）"),
    ("撤销", 90, "撤销上一步（键 Z）"),
)

# 声源种类那一排：**发的是一个号，不是一次按键** —— `P <号>`，和建模器那排图元一个走法。号是两边
# 约好的（`宿主/audio/main.cpp` 的 `Armed`、`宿主/editor_host.h` 的 `P` 说明）。一次性：放一个就
# 回到选择；要再放一个，再点一下这里。
_AUDIO_SOUNDS = (
    ("音调", 1, "手里拿上一段音调（440 Hz 半秒），再在画面上左键点一下就放在那里"),
    ("噪声", 2, "手里拿上一段噪声（半秒），再在画面上左键点一下就放在那里"),
    ("音效", 3, "手里拿上一段合成音效（默认一声下滑的激光），再在画面上左键点一下就放在那里"),
)

# 那个「音效…」对话框里每一栏的（初值、范围、步长、小数位）。**顺序就是发给程序的顺序** ——
# 对话框回填 `_audio_synth` 也是这个顺序（波形单独在最前）。
_AUDIO_SYNTH_FIELDS = (
    ("f0", "起始频率 Hz", 1200.0, 1.0, 20000.0, 10.0, 1),
    ("f1", "结束频率 Hz", 300.0, 1.0, 20000.0, 10.0, 1),
    ("seconds", "长度 秒", 0.22, 0.02, 30.0, 0.01, 3),
    ("attack", "起音 秒", 0.004, 0.0, 10.0, 0.005, 3),
    ("decay", "衰减 秒", 0.06, 0.0, 10.0, 0.005, 3),
    ("sustain", "持续电平 0..1", 0.25, 0.0, 1.0, 0.05, 2),
    ("release", "释音 秒", 0.12, 0.0, 10.0, 0.005, 3),
    ("cutoff", "低通 Hz（0 = 关）", 0.0, 0.0, 20000.0, 100.0, 0),
)

# 参数那一排：**发的是 `V <名字> <数>`** —— 音量是个值，敲一下键带不走它。**带符号的是"加这个
# 数"**（面板不知道当前值是多少，所以能加减的就说成加减）；不带符号的（频率、长度）走弹窗问一个
# 绝对值，那两条在 `_audio_frequency` / `_audio_length` 里。
_AUDIO_VALUES = (
    ("响一点", ("gain", 0.1), "选中声源的音量 +0.1"),
    ("轻一点", ("gain", -0.1), "选中声源的音量 -0.1"),
    ("整场响", ("master", 0.1), "整场音量 +0.1"),
    ("整场轻", ("master", -0.1), "整场音量 -0.1"),
    ("参考距离+", ("reference", 0.5), "参考距离 +0.5（这个距离以内都是满音量）"),
    ("参考距离-", ("reference", -0.5), "参考距离 -0.5"),
    ("最远+", ("max", 2.0), "最大距离 +2（再远就听不见了）"),
    ("最远-", ("max", -2.0), "最大距离 -2"),
)

# A window's palette, close enough to VS Code's dark theme that the gutter and the code agree.
_STYLE = """
QMainWindow, QWidget { background: #1e1e1e; color: #d4d4d4; }
QTreeView, QPlainTextEdit, QTabWidget::pane {
    background: #1e1e1e; color: #d4d4d4; border: 0;
}
QTreeView::item:selected, QTabBar::tab:selected { background: #094771; }
QTabBar::tab { background: #2d2d2d; color: #9da5b4; padding: 4px 12px; border: 0; }
QHeaderView::section { background: #252526; color: #9da5b4; border: 0; padding: 3px; }
QMenuBar, QMenu, QToolBar, QStatusBar { background: #252526; color: #d4d4d4; border: 0; }
QMenu::item:selected { background: #094771; }
QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; padding: 4px 10px; }
QPushButton:disabled { color: #5a5a5a; }
QSplitter::handle { background: #252526; }
QLineEdit { background: #3c3c3c; color: #d4d4d4; border: 1px solid #3c3c3c; padding: 3px 6px; }
QToolTip { background: #252526; color: #d4d4d4; border: 0; }
"""


class EditorWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("烛龙 编码器")
        self.resize(1280, 820)
        self.setStyleSheet(_STYLE)

        self._settings = QSettings()
        # **两个不同的"根"，不要合成一个。**
        #
        #   `_root`     文件树现在指着哪儿。用户点一下"打开文件夹"它就变了。
        #   `_package`  这份软件自己住在哪儿。**永不改变** —— 编译器、内核库、项目清单都在这儿。
        #
        # 合成一个的后果是把编译器挂在"你正在看的那个文件夹"上：打开 `宿主/xiangqi` 去看象棋的
        # 源码，编译器就跟着不见了（"没有编译器 · 找的是 …\宿主\xiangqi\运行环境\zig\zig.exe"）。
        # 编译器和内核属于整份包，不属于你在看的那一格。
        self._root = project_root()
        self._package = project_root()
        self._font_size = 11
        # Looked for when a C++ run needs it rather than at startup: finding it is a file check plus,
        # for a source checkout, a compiler probe, and a window that takes a second to appear is a
        # window that is slow. A miss is not remembered -- see `_find_toolchain`.
        self._toolchain: "cpp.Toolchain | None" = None
        self._project: cpp.Project | None = None
        # Set while a build is going, so its exit can be told from the program's.
        self._after_build: tuple[Path, Path] | None = None
        # Set while packaging, so its exit can be told from the program's too.
        self._packing: packager.Plan | None = None
        # 正在跑的是不是建模器 / 音频器 —— 决定画面画到哪块面板上、声源表落到哪个大纲里。
        # 见 `_active_view` 和 `_on_scene`。
        self._model_running = False
        self._audio_running = False

        self._runner = Runner(self)
        self._runner.output.connect(self._on_output)
        self._runner.frame.connect(self._on_frame)
        self._runner.scene.connect(self._on_scene)
        self._runner.finished.connect(self._on_finished)

        # A compiled program presents where it was written to present, so the editor watches for its
        # window and copies what is on it. See `window.py`.
        self._mirror = Mirror(self)
        self._mirror.frame.connect(self._on_mirrored)

        self._build_actions()
        self._build_layout()
        self._build_menus()

        self._refresh_backend_note()
        self._restore_session()
        self._update_actions()

    # -- construction ----------------------------------------------------------------------

    def _build_actions(self) -> None:
        def action(text: str, slot, shortcut: str | QKeySequence | None = None) -> QAction:
            made = QAction(text, self)
            if shortcut is not None:
                made.setShortcut(shortcut)
            made.triggered.connect(slot)
            return made

        self.act_new = action("新建", lambda: self._new_file(name="untitled" + self._new_suffix()),
                              QKeySequence.StandardKey.New)
        self.act_open = action("打开文件…", self._open_file, QKeySequence.StandardKey.Open)
        self.act_open_folder = action("打开文件夹…", self._open_folder)
        self.act_save = action("保存", self._save, QKeySequence.StandardKey.Save)
        self.act_save_as = action("另存为…", self._save_as, QKeySequence.StandardKey.SaveAs)
        self.act_close_tab = action("关闭标签", lambda: self._close_tab(self._tabs.currentIndex()),
                                    QKeySequence.StandardKey.Close)
        self.act_quit = action("退出", self.close, QKeySequence.StandardKey.Quit)

        # 三页，互斥 —— **这就是"进入 3D 建模 / 音频"的入口**。放在工具栏最左边，见 `_build_menus`。
        self.act_page_code = action("代码", lambda: self._show_page(_PAGE_CODE))
        self.act_page_model = action("3D 建模", lambda: self._show_page(_PAGE_MODEL))
        self.act_page_audio = action("音频", lambda: self._show_page(_PAGE_AUDIO))
        self._page_group = QActionGroup(self)
        self._page_group.setExclusive(True)
        for made in (self.act_page_code, self.act_page_model, self.act_page_audio):
            made.setCheckable(True)
            self._page_group.addAction(made)
        self.act_page_code.setChecked(True)

        self.act_undo = action("撤销", lambda: self._on_editor("undo"),
                               QKeySequence.StandardKey.Undo)
        self.act_redo = action("重做", lambda: self._on_editor("redo"),
                               QKeySequence.StandardKey.Redo)
        self.act_cut = action("剪切", lambda: self._on_editor("cut"),
                              QKeySequence.StandardKey.Cut)
        self.act_copy = action("复制", lambda: self._on_editor("copy"),
                               QKeySequence.StandardKey.Copy)
        self.act_paste = action("粘贴", lambda: self._on_editor("paste"),
                                QKeySequence.StandardKey.Paste)
        self.act_find = action("查找", lambda: self._open_find(False), QKeySequence("Ctrl+F"))
        self.act_replace = action("替换", lambda: self._open_find(True), QKeySequence("Ctrl+H"))
        self.act_find_next = action("查找下一个", lambda: self._find(False), QKeySequence("F3"))
        self.act_find_previous = action("查找上一个", lambda: self._find(True),
                                        QKeySequence("Shift+F3"))
        self.act_goto = action("转到行…", self._goto_line, QKeySequence("Ctrl+G"))
        self.act_comment = action("注释", lambda: self._on_editor("toggle_comment"),
                                  QKeySequence("Ctrl+/"))
        self.act_duplicate = action("复制行", lambda: self._on_editor("duplicate_lines"),
                                    QKeySequence("Ctrl+D"))
        self.act_move_up = action("上移行", lambda: self._on_editor("move_lines", -1),
                                  QKeySequence("Alt+Up"))
        self.act_move_down = action("下移行", lambda: self._on_editor("move_lines", 1),
                                    QKeySequence("Alt+Down"))
        self.act_zoom_in = action("放大", lambda: self._zoom(+1), QKeySequence("Ctrl+="))
        self.act_zoom_out = action("缩小", lambda: self._zoom(-1), QKeySequence("Ctrl+-"))
        self.act_zoom_reset = action("复位字号", lambda: self._zoom(0), QKeySequence("Ctrl+0"))

        self.act_run = action("运行", self._run, QKeySequence("F5"))
        self.act_stop = action("停止", self._stop, QKeySequence("Shift+F5"))
        self.act_stop.setEnabled(False)
        self.act_pack = action("打包游戏", self._pack_game, QKeySequence("Ctrl+Shift+B"))
        self.act_clear = action("清空输出", lambda: self._console.clear())

        self.act_templates = [
            action(name, lambda checked=False, source=source, suffix=suffix:
                   self._new_file(name="untitled" + suffix, text=source))
            for name, source, suffix in (
                (*templates.EMPTY, ".py"), (*templates.SCENE, ".py"), (*templates.HOST, ".py"),
                (*templates.CPP, ".cpp"), (*templates.SONG, ".song"))
        ]

    def _build_layout(self) -> None:
        self._tree_model = QFileSystemModel(self)
        self._tree_model.setRootPath(str(self._root))
        self._tree_model.setFilter(
            QDir.Filter.AllDirs | QDir.Filter.Files | QDir.Filter.NoDotAndDotDot
        )

        self._tree = QTreeView()
        self._tree.setModel(self._tree_model)
        self._tree.setRootIndex(self._tree_model.index(str(self._root)))
        self._tree.setAnimated(False)
        self._tree.setIndentation(14)
        self._tree.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self._tree.doubleClicked.connect(self._on_tree_activated)
        for column in range(1, 4):
            self._tree.hideColumn(column)

        self._findbar = FindBar()
        self._findbar.hide()
        self._findbar.matches.connect(self._on_matches)
        self._findbar.closed.connect(self._on_find_closed)

        self._tabs = QTabWidget()
        self._tabs.setTabsClosable(True)
        self._tabs.setMovable(True)
        self._tabs.setDocumentMode(True)
        self._tabs.tabCloseRequested.connect(self._close_tab)
        self._tabs.currentChanged.connect(self._on_tab_changed)

        editor_area = QWidget()
        editor_layout = QVBoxLayout(editor_area)
        editor_layout.setContentsMargins(0, 0, 0, 0)
        editor_layout.setSpacing(0)
        editor_layout.addWidget(self._findbar)
        editor_layout.addWidget(self._tabs)

        self._console = Console()
        self._console.jumpRequested.connect(self._jump_to)

        # The panel on the right shows what a run drew. Its header carries the frame's size, which is
        # the one thing about a picture the picture cannot tell you.
        self._viewport = Viewport()
        self._viewport.key.connect(self._on_view_key)
        self._viewport.moved.connect(self._on_view_moved)
        self._viewport.clicked.connect(self._on_view_clicked)
        self._viewport.wheeled.connect(self._on_view_wheeled)
        self._frame_label = QLabel("")
        render_area = QWidget()
        render_layout = QVBoxLayout(render_area)
        render_layout.setContentsMargins(6, 6, 6, 6)
        render_layout.setSpacing(4)
        render_header = QHBoxLayout()
        render_header.addWidget(QLabel("画面"))
        render_header.addStretch(1)
        render_header.addWidget(self._frame_label)
        render_layout.addLayout(render_header)
        render_layout.addWidget(self._viewport)

        top = QSplitter(Qt.Orientation.Horizontal)
        top.addWidget(self._tree)
        top.addWidget(editor_area)
        top.addWidget(render_area)
        top.setStretchFactor(0, 0)
        top.setStretchFactor(1, 3)
        top.setStretchFactor(2, 2)
        top.setSizes([240, 820, 620])

        bottom = QWidget()
        bottom_layout = QVBoxLayout(bottom)
        bottom_layout.setContentsMargins(6, 6, 6, 6)
        bottom_layout.setSpacing(4)
        header = QHBoxLayout()
        header.addWidget(QLabel("输出"))
        header.addStretch(1)
        self._run_label = QLabel("")
        header.addWidget(self._run_label)
        bottom_layout.addLayout(header)
        bottom_layout.addWidget(self._console)

        # 三页共用底下那个控制台：**输出在哪一页都看得见**。建模器说的话（"面 1 / 共 6 个"、
        # "移动完成…"）和音频器说的话（"放下：音调 1，在 …"）都落在那里，边做边看着它。
        self._pages = QStackedWidget()
        self._pages.addWidget(top)                       # 0：代码
        self._pages.addWidget(self._build_model_page())  # 1：3D 建模
        self._pages.addWidget(self._build_audio_page())  # 2：音频

        splitter = QSplitter(Qt.Orientation.Vertical)
        splitter.addWidget(self._pages)
        splitter.addWidget(bottom)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([600, 200])
        self.setCentralWidget(splitter)

        self._position = QLabel("")
        self._backend_label = QLabel("")
        self.statusBar().addPermanentWidget(self._position)
        self.statusBar().addPermanentWidget(self._backend_label)

    # -- the modelling page ------------------------------------------------------------------

    def _build_model_page(self) -> QWidget:
        """建模那一页：一块大画面，一条工具，一句提示。

        工具条上的按钮**不自己实现任何东西** —— 它们替用户敲一下键，程序那头（`宿主/modeler`）
        照常把它当键盘。所以这一页和"打开一个 `.cpp` 按 F5，用键盘操作它"是同一条路，只是排得
        顺手一点；协议在 `05-编码器.md` 里，两边各有一份表。
        """
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(6, 6, 6, 6)
        layout.setSpacing(4)

        strip = QHBoxLayout()
        self._model_tools: list[QPushButton] = []
        for label, key, tip in _MODEL_BUTTONS:
            button = QPushButton(label)
            button.setToolTip(tip)
            button.clicked.connect(lambda _checked=False, code=key: self._on_model_tool(code))
            self._model_tools.append(button)
            strip.addWidget(button)
        strip.addStretch(1)
        self._model_colour = QPushButton("上色")
        self._model_colour.setToolTip("给选中的东西挑一个颜色：对象模式涂整个对象，面模式涂那个面")
        self._model_colour.clicked.connect(self._colour_model)
        self._model_tools.append(self._model_colour)
        strip.addWidget(self._model_colour)
        self._model_texture = QPushButton("贴图…")
        self._model_texture.setToolTip("给选中的面挂一张 BMP（挂的是它那种材质，所以同材质的几个面都会挂上）")
        self._model_texture.clicked.connect(self._texture_model)
        self._model_tools.append(self._model_texture)
        strip.addWidget(self._model_texture)
        self._model_picture = QPushButton("插入图片")
        self._model_picture.setToolTip("往场景里放一张图片：一块贴着它的平面对象（放在画面中央）")
        self._model_picture.clicked.connect(self._insert_picture)
        self._model_tools.append(self._model_picture)
        strip.addWidget(self._model_picture)
        self._model_open = QPushButton("打开…")
        self._model_open.setToolTip("读一个 .model 进来")
        self._model_open.clicked.connect(self._open_model)
        self._model_tools.append(self._model_open)
        strip.addWidget(self._model_open)
        self._model_import = QPushButton("导入…")
        self._model_import.setToolTip("把别人的网格文件（OBJ）读进来；导入是有损的，控制台会说改了什么")
        self._model_import.clicked.connect(self._import_model)
        self._model_tools.append(self._model_import)
        strip.addWidget(self._model_import)
        self._model_save = QPushButton("保存")
        self._model_save.setToolTip("存回原处；还没存过就先问存到哪儿")
        self._model_save.clicked.connect(self._save_model)
        self._model_tools.append(self._model_save)
        strip.addWidget(self._model_save)
        self._model_export = QPushButton("导出场景")
        self._model_export.setToolTip("把整个场景导出一个 .scene（游戏直接读）：每个对象存成同级的 .model")
        self._model_export.clicked.connect(self._export_scene)
        self._model_tools.append(self._model_export)
        strip.addWidget(self._model_export)
        self._model_start = QPushButton("开始建模")
        self._model_start.setToolTip("起一个建模器子进程：画面走这块面板，鼠标键盘也走它")
        self._model_start.clicked.connect(self._start_model)
        strip.addWidget(self._model_start)
        layout.addLayout(strip)

        palette = QHBoxLayout()
        palette.addWidget(QLabel("图元："))
        for label, kind, tip in _MODEL_PRIMITIVES:
            button = QPushButton(label)
            button.setToolTip(f"{tip}：先点这里，再在画面上左键点一下")
            button.clicked.connect(
                lambda _checked=False, k=kind: self._on_model_primitive(k))
            self._model_tools.append(button)
            palette.addWidget(button)
        palette.addStretch(1)
        layout.addLayout(palette)

        self._model_view = Viewport()
        self._model_view.set_fill(True)   # 建模画布要**铺满**，窗口变形状图也跟着铺满，不留黑边
        for signal, slot in ((self._model_view.key, self._on_view_key),
                             (self._model_view.moved, self._on_view_moved),
                             (self._model_view.clicked, self._on_view_clicked),
                             (self._model_view.wheeled, self._on_view_wheeled)):
            signal.connect(slot)
        # 左边一列**对象**（大纲），右边是画面 —— 和 Blender 的 Outliner 一个位置。
        self._model_scene: list[tuple[int, str]] = []
        self._model_active = -1
        self._model_syncing = False
        self._model_objects = QListWidget()
        self._model_objects.setToolTip("场景里的对象。点一个就选中它（视口里会亮），再切到面/点模式编它。")
        self._model_objects.currentRowChanged.connect(self._on_model_row)
        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(self._model_objects)
        split.addWidget(self._model_view)
        split.setStretchFactor(1, 1)
        split.setSizes([170, 630])
        layout.addWidget(split, 1)

        hint = QHBoxLayout()
        hint_label = QLabel("转：左键拖空处 或 右键　·　平移：Shift+拖空处 或 中键　·　"
                            "缩放：滚轮（选中对象时缩它）　·　选：左键点，**Shift 点 = 加选**　·　"
                            "拖：点在东西上按住，拖动中按 X/Y/Z 锁轴　·　滚轮缩画面：Shift+滚轮　｜　"
                            "图元：先点一个形状，再左键放到画面上（Esc 取消）　｜　"
                            "键：1 面 · 2 点 · 3 对象 · R 转 · T 缩放 · V 复制 · J 合并 · F 落下 · "
                            "W 焊接 · U 并集 · I 交集 · K 差集 · E 挤出 · D 细分 · B 切角 · "
                            "C 硬边 · X 删除 · Y 坐标轴 · Z 撤销")
        hint_label.setWordWrap(True)   # 不换行的话，这一长条的最小宽度会把整页撑得极宽、反挤画布
        hint.addWidget(hint_label)
        hint.addStretch(1)
        self._model_frame_label = QLabel("")
        hint.addWidget(self._model_frame_label)
        layout.addLayout(hint)

        self._model_tools_enabled(False)
        return page

    # -- the audio page ----------------------------------------------------------------------

    def _build_audio_page(self) -> QWidget:
        """音频那一页：一块画面、左边一列声源、三排工具，一句提示。

        和建模页是同一个走法：按钮**不实现任何东西**，只替用户敲一下键（`K`）或者发一个值（`V` /
        `P`）。区别只在编排 —— 工具排成"键 / 声源 / 参数 / 文件"几排。
        """
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(6, 6, 6, 6)
        layout.setSpacing(4)

        strip = QHBoxLayout()
        self._audio_tools: list[QPushButton] = []
        for label, key, tip in _AUDIO_BUTTONS:
            button = QPushButton(label)
            button.setToolTip(tip)
            button.clicked.connect(lambda _checked=False, code=key: self._on_audio_key(code))
            self._audio_tools.append(button)
            strip.addWidget(button)
        strip.addStretch(1)
        self._audio_play = QPushButton("重开音频器")
        self._audio_play.setToolTip("起一个音频器子进程：画面走这块面板，声音走声卡，鼠标键盘也走它")
        self._audio_play.clicked.connect(self._start_audio)
        strip.addWidget(self._audio_play)
        layout.addLayout(strip)

        # 一排声源种类：发的是 `P <号>`，和建模器那排图元一个走法。
        sounds = QHBoxLayout()
        sounds.addWidget(QLabel("声源："))
        for label, kind, tip in _AUDIO_SOUNDS:
            button = QPushButton(label)
            button.setToolTip(f"{tip}：先点这里，再在画面上左键点一下")
            button.clicked.connect(lambda _checked=False, k=kind: self._on_audio_sound(k))
            self._audio_tools.append(button)
            sounds.addWidget(button)
        sounds.addStretch(1)
        layout.addLayout(sounds)

        # 一排参数：发的是 `V <名字> <数>`。
        values = QHBoxLayout()
        values.addWidget(QLabel("参数："))
        for label, (name, amount), tip in _AUDIO_VALUES:
            button = QPushButton(label)
            button.setToolTip(tip)
            button.clicked.connect(lambda _checked=False, n=name, a=amount: self._send_value(n, a))
            self._audio_tools.append(button)
            values.addWidget(button)
        self._audio_freq = QPushButton("频率…")
        self._audio_freq.setToolTip("把选中音调的频率设成一个数（Hz）")
        self._audio_freq.clicked.connect(self._audio_frequency)
        self._audio_tools.append(self._audio_freq)
        values.addWidget(self._audio_freq)
        self._audio_length = QPushButton("长度…")
        self._audio_length.setToolTip("把选中声源的长度设成一个数（秒）")
        self._audio_length.clicked.connect(self._audio_length_dialog)
        self._audio_tools.append(self._audio_length)
        values.addWidget(self._audio_length)
        self._audio_synth_button = QPushButton("音效…")
        self._audio_synth_button.setToolTip(
            "把选中的**合成音效**调一遍：波形、起止频率、长度、包络（起音/衰减/持续/释音）、低通")
        self._audio_synth_button.clicked.connect(self._audio_synth_dialog)
        self._audio_tools.append(self._audio_synth_button)
        values.addWidget(self._audio_synth_button)
        self._audio_global = QPushButton("全局")
        self._audio_global.setToolTip(
            "让选中的声源**不跟位置走**（满音量、居中）—— 背景音乐就该这样；再按一下变回世界里的声源（键 G）")
        self._audio_global.clicked.connect(lambda: self._send_value("global"))
        self._audio_tools.append(self._audio_global)
        values.addWidget(self._audio_global)
        values.addStretch(1)
        layout.addLayout(values)

        files = QHBoxLayout()
        files.addWidget(QLabel("文件："))
        self._audio_open = QPushButton("打开…")
        self._audio_open.setToolTip("读一个 .scene，把它声音那一半变成声源")
        self._audio_open.clicked.connect(self._open_audio_scene)
        self._audio_tools.append(self._audio_open)
        files.addWidget(self._audio_open)
        self._audio_import = QPushButton("导入 WAV…")
        self._audio_import.setToolTip("读一个 WAV 放进世界（循环打开）")
        self._audio_import.clicked.connect(self._import_wav)
        self._audio_tools.append(self._audio_import)
        files.addWidget(self._audio_import)
        self._audio_song = QPushButton("导入音乐…")
        self._audio_song.setToolTip(
            "读一份 .song（一首曲子）放进世界 —— **默认循环、默认全局**（背景音乐不跟位置走）")
        self._audio_song.clicked.connect(self._import_song)
        self._audio_tools.append(self._audio_song)
        files.addWidget(self._audio_song)
        self._audio_save = QPushButton("保存")
        self._audio_save.setToolTip("把这一份存成一个 .scene")
        self._audio_save.clicked.connect(self._save_audio_scene)
        self._audio_tools.append(self._audio_save)
        files.addWidget(self._audio_save)
        self._audio_export = QPushButton("导出 WAV")
        self._audio_export.setToolTip("把当前这一份整体混成一个 .wav（不影响正在响的那一路）")
        self._audio_export.clicked.connect(self._export_wav)
        self._audio_tools.append(self._audio_export)
        files.addWidget(self._audio_export)
        files.addStretch(1)
        layout.addLayout(files)

        self._audio_view = Viewport()
        self._audio_view.set_fill(True)   # 画面按面板形状出，所以铺满，不留黑边
        for signal, slot in ((self._audio_view.key, self._on_view_key),
                             (self._audio_view.moved, self._on_view_moved),
                             (self._audio_view.clicked, self._on_view_clicked),
                             (self._audio_view.wheeled, self._on_view_wheeled)):
            signal.connect(slot)
        # 左边一列**声源**（大纲），右边是画面 —— 和建模页一个位置。
        self._audio_sources: list[tuple[int, str]] = []
        self._audio_active = -1
        self._audio_syncing = False
        # 活动音效那份配方：波形 + 八个参数。程序每次把活动音效的参数**回推**过来就更新它，
        # 对话框打开时用它填初值（默认那组 = 程序放下一个「音效」时的那份 zap）。
        self._audio_synth = (2, 1200.0, 300.0, 0.22, 0.004, 0.06, 0.25, 0.12, 0.0)
        # 音符格子那一份：宿主推过来的（速度、一遍拍数、乐器数）+ 一串音符。`_timeline_has_song`
        # 决定那块面板出不出来 —— 选中的是曲子才有。
        self._timeline_song = (120.0, 4.0, 1)
        self._timeline_notes: list[tuple[float, float, int, float, int]] = []
        self._timeline_instruments: list[tuple] = []
        self._timeline_has_song = False
        self._reporting_play = False
        self._instrument_dialog: InstrumentDialog | None = None
        self._audio_objects = QListWidget()
        self._audio_objects.setToolTip("世界里的声源。点一个就选中它，再拖它 / 改它的音量、距离。")
        self._audio_objects.currentRowChanged.connect(self._on_audio_row)
        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(self._audio_objects)
        split.addWidget(self._audio_view)
        split.setStretchFactor(1, 1)
        split.setSizes([170, 630])

        # **音符格子**：选中的是一首曲子的时候才出来。它画的是宿主推过来的那份曲子（模型在 C++ 那头
        # —— 见 `timeline.py`），鼠标动作走 `command` → 一行 `V …` 写回去。放在下面一条，默认矮一点；
        # 想大就拖那条分隔线。
        controls = QHBoxLayout()
        controls.addWidget(QLabel("音符格子："))
        controls.addWidget(QLabel("速度"))
        self._timeline_tempo = QDoubleSpinBox()
        self._timeline_tempo.setRange(20.0, 400.0)
        self._timeline_tempo.setDecimals(1)
        self._timeline_tempo.setSingleStep(1.0)
        self._timeline_tempo.setToolTip("速度（BPM）——回车或者点别处就生效")
        self._timeline_tempo.editingFinished.connect(self._on_tempo_edited)
        controls.addWidget(self._timeline_tempo)
        controls.addWidget(QLabel("BPM"))
        controls.addWidget(QLabel("一遍"))
        self._timeline_loop = QDoubleSpinBox()
        self._timeline_loop.setRange(0.25, 256.0)
        self._timeline_loop.setDecimals(2)
        self._timeline_loop.setSingleStep(1.0)
        self._timeline_loop.setToolTip("一遍多少拍 —— 渲染出来的长度就是它")
        self._timeline_loop.editingFinished.connect(self._on_loop_edited)
        controls.addWidget(self._timeline_loop)
        controls.addWidget(QLabel("拍"))
        self._timeline_save = QPushButton("保存曲子")
        self._timeline_save.setToolTip("把格子上改的这一份写成 .song（不按它，磁盘上那份还是旧的）")
        self._timeline_save.clicked.connect(self._save_song)
        controls.addWidget(self._timeline_save)
        self._timeline_instruments_button = QPushButton("乐器…")
        self._timeline_instruments_button.setToolTip(
            "编辑这首曲子的乐器：波形、包络（起音/衰减/持续/释音）、增益、低通 —— 改一下就听得到")
        self._timeline_instruments_button.clicked.connect(self._open_instruments)
        controls.addWidget(self._timeline_instruments_button)
        controls.addStretch(1)
        self._timeline_hint = QLabel("")
        controls.addWidget(self._timeline_hint)
        for made in (self._timeline_tempo, self._timeline_loop, self._timeline_save,
                     self._timeline_instruments_button):
            self._audio_tools.append(made)

        self._audio_timeline = TimelineView()
        self._audio_timeline.command.connect(self._on_timeline_command)
        panel = QWidget()
        panel_layout = QVBoxLayout(panel)
        panel_layout.setContentsMargins(0, 0, 0, 0)
        panel_layout.setSpacing(2)
        panel_layout.addLayout(controls)
        panel_layout.addWidget(self._audio_timeline, 1)
        self._audio_timeline_panel = panel
        panel.setVisible(False)

        vertical = QSplitter(Qt.Orientation.Vertical)
        vertical.addWidget(split)
        vertical.addWidget(panel)
        vertical.setStretchFactor(0, 3)
        vertical.setStretchFactor(1, 1)
        vertical.setSizes([400, 200])
        layout.addWidget(vertical, 1)

        hint = QHBoxLayout()
        hint_label = QLabel("听者 = 相机：**转视角就是转头**（左右一转，声像跟着走）　·　"
                            "左键 选/拖声源（拖着挪 = 换声像、换远近）　·　右键拖动 转视角　·　"
                            "中键拖动 平移　·　滚轮 远近　｜　"
                            "键：空格 播放/暂停 · L 循环 · M 静音 · G 全局 · X 删除 · ↑↓ 换一个 · Z 撤销　｜　"
                            "「音调」「噪声」「音效」点一下拿到手里，再左键放到画面上；"
                            "选中一个音效，用「音效…」调它的波形/包络　｜　"
                            "背景音乐：从菜单新建一份「曲子」，写好后用「导入音乐…」放进来（默认循环、全局）")
        hint_label.setWordWrap(True)   # 不换行的话，这一长条的最小宽度会把整页撑得极宽、反挤画布
        hint.addWidget(hint_label)
        hint.addStretch(1)
        self._audio_frame_label = QLabel("")
        hint.addWidget(self._audio_frame_label)
        layout.addLayout(hint)

        self._audio_tools_enabled(False)
        return page

    def _show_page(self, index: int) -> None:
        """切页。切到工具那一页时顺手把那个工具起起来 —— 进去就是能用的。"""
        self._pages.setCurrentIndex(index)
        (self.act_page_code, self.act_page_model, self.act_page_audio)[index].setChecked(True)
        if index == _PAGE_MODEL:
            start, view, button = self._start_model, self._model_view, self._model_start
        elif index == _PAGE_AUDIO:
            start, view, button = self._start_audio, self._audio_view, self._audio_play
        else:
            return
        if not self._runner.running():
            start()
        else:
            # 有东西在跑（一道 Run 或者上一次的工具）。**不抢** —— 悄悄把别人踩掉是最难查的
            # 那种意外；想换就自己按那个「开始 / 重开」按钮。
            button.setToolTip("有东西在跑。按「停止」再进来，或者直接点这里换掉它")
        if view.isVisible():
            view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _start_model(self) -> None:
        """起建模器。

        和跑一个 `.cpp` 是同一条路：子进程，画面从 stderr 回来、鼠标键盘从 stdin 过去。**不一样
        的是不用编** —— 它按成品启动（包里那个是打包时装好的），所以进去就是画的。
        """
        program = self._find_modeler()
        if program is None:
            self._console.append_line("[没有建模器] 找的是这两条：")
            for parts in _MODELER_PATHS:
                self._console.append_line(f"  {self._package.joinpath(*parts)}")
            self._console.append_line("  源码树里跑一次 `python 打包.py` 就有了")
            return

        try:
            arguments = split_arguments(self._arguments.text())
        except ValueError as problem:
            QMessageBox.warning(self, "参数读不了", str(problem))
            return
        arguments = self._render_size(arguments, self._model_view)

        # 先记下"这一轮画在建模那一页上"，`_start` 才知道该擦哪块面板。
        self._model_running = True
        self._model_objects.clear()   # 上一轮的对象表作废；新的一起来会自己填
        self._model_scene = []
        self._model_active = -1
        self._start(f"$ {program.name} {' '.join(arguments)}".rstrip(), _RUNNING)
        self._runner.run(program_command(program, arguments, self._package))
        # 记住过哪个模型就顺手读进来 —— 进这一页，上次那个东西就在。**现在发出去没关系**：这几行
        # 躺在管子里，子进程起来之后自己会读到。
        remembered = self._model_path()
        if remembered:
            self._runner.write(f"L {remembered}\n")
        self._model_view.set_capturing(True)
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)
        self._model_tools_enabled(True)
        self._model_start.setText("重开建模")

    def _render_size(self, arguments: list[str], view: Viewport) -> list[str]:
        """让那个工具**按这块面板的形状**出图，而不是拿一个固定的 800×500 去塞。

        固定的后果就是两边留黑边：面板越宽，黑边越宽，看上去就是"这块画面很小，没铺满"。按面板
        的形状出图，图就铺到底、铺到边（两个工具共用这一套）。

        **像素总数不涨**：保持工具自己那个 `800×500` 的份量，只把形状换成面板的。那一套默认值是
        量着帧率挑的（见 `宿主/modeler` 开头那张表：800×500 是 54 帧档，1024×640 掉到 34 帧），
        而像素是软件光栅化出来的 —— 想更清楚就把 `_MODEL_PIXELS` 调大，代价是帧率，一句话的事。

        自己在参数框里写了 `--width` / `--height` 的，一个字都不加 —— 那是用户明说的。
        """
        if any(a.startswith("--width") or a.startswith("--height") for a in arguments):
            return arguments
        room = view.room()
        if room.isEmpty():
            return arguments
        pixels = room.width() * room.height()
        shrink = min(1.0, (_MODEL_PIXELS / pixels) ** 0.5)
        width = max(64, int(room.width() * shrink))
        height = max(64, int(room.height() * shrink))
        return arguments + ["--width", str(width), "--height", str(height)]

    def _find_modeler(self) -> Path | None:
        """建模器在哪，或者 None。两个地方按顺序找，见 `_MODELER_PATHS`。"""
        for parts in _MODELER_PATHS:
            candidate = self._package.joinpath(*parts)
            if candidate.is_file():
                return candidate
        return None

    def _start_audio(self) -> None:
        """起音频器。

        和起建模器是同一条路：子进程，画面从 stderr 回来、鼠标键盘从 stdin 过去，按成品启动、
        **不用编** —— 包里那个是打包时装好的。区别是它会出声：混音走声卡，见 `宿主/audio/main.cpp`。
        """
        program = self._find_audio()
        if program is None:
            self._console.append_line("[没有音频器] 找的是这两条：")
            for parts in _AUDIO_PATHS:
                self._console.append_line(f"  {self._package.joinpath(*parts)}")
            self._console.append_line("  源码树里跑一次 `python 打包.py` 就有了")
            return

        try:
            arguments = split_arguments(self._arguments.text())
        except ValueError as problem:
            QMessageBox.warning(self, "参数读不了", str(problem))
            return
        arguments = self._render_size(arguments, self._audio_view)

        # 先记下"这一轮画在音频那一页上"，`_start` 才知道该擦哪块面板、`_on_scene` 才知道声源表
        # 落到哪个大纲里。
        self._audio_running = True
        self._audio_sources = []
        self._audio_active = -1
        self._audio_objects.clear()
        # 音符格子跟着上一轮作废；新的一起来会自己填（见 `_apply_timeline`）。
        self._timeline_notes = []
        self._timeline_instruments = []
        self._timeline_has_song = False
        self._reporting_play = False
        self._audio_timeline.clear()
        self._audio_timeline_panel.setVisible(False)
        self._close_instruments()
        self._start(f"$ {program.name} {' '.join(arguments)}".rstrip(), _RUNNING)
        self._runner.run(program_command(program, arguments, self._package))
        # 记住过哪个场景就顺手读进来 —— 进这一页，上次那份就在。这几行躺在管子里，子进程起来之后
        # 自己会读到。
        remembered = self._audio_scene_path()
        if remembered:
            self._runner.write(f"L {remembered}\n")
        self._audio_view.set_capturing(True)
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)
        self._audio_tools_enabled(True)
        self._audio_play.setText("重开音频器")

    def _find_audio(self) -> Path | None:
        """音频器在哪，或者 None。和 `_find_modeler` 一个顺序，见 `_AUDIO_PATHS`。"""
        for parts in _AUDIO_PATHS:
            candidate = self._package.joinpath(*parts)
            if candidate.is_file():
                return candidate
        return None

    # -- 音频页上的那些按钮 -------------------------------------------------------------------
    #
    # 按键走 `K`，声源种类走 `P <号>`，参数走 `V <名字> <数>`，文件走 `S` / `L` / `I` / `E`。
    # 协议的两头分别在 `宿主/editor_host.h` 的 `consume` 和这一页这几个方法 —— 改一处要改两处。

    def _on_audio_key(self, key: int) -> None:
        self._runner.write(f"K {key} 1\n")
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _on_audio_sound(self, kind: int) -> None:
        self._runner.write(f"P {kind}\n")
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _send_value(self, name: str, *numbers: float) -> None:
        """`V <名字> <数…>`。带符号的是"加这个数"，程序那头按加减理解（见 `ApplyValue`）。

        不给数也对（`V global` 就是一个翻转开关）—— 那样行尾就不该留一个空格。
        """
        parts = " ".join(f"{number:g}" for number in numbers)
        self._runner.write(f"V {name} {parts}".rstrip() + "\n")
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _audio_frequency(self) -> None:
        value, ok = QInputDialog.getDouble(
            self, "频率", "选中音调的频率（Hz）：", 440.0, 20.0, 20000.0, 1)
        if ok:
            self._send_value("freq", value)

    def _audio_length_dialog(self) -> None:
        value, ok = QInputDialog.getDouble(
            self, "长度", "选中声源的长度（秒）：", 0.5, 0.02, 30.0, 2)
        if ok:
            self._send_value("length", value)

    def _audio_synth_dialog(self) -> None:
        """把选中的**合成音效**调一遍。一趟出去是五条 `V`：波形、起止频率、长度、包络、低通。

        初值取程序**回推**过来的那份配方（`_on_audio_scene` 的 `synth` 那行）—— 所以打开一个存过的
        场景再打开这个框，显示的是文件里那份，不是面板上一次敲的。改完立刻重渲染，能直接听。
        """
        wave, *rest = self._audio_synth
        dialog = QDialog(self)
        dialog.setWindowTitle("音效（合成）")
        form = QFormLayout(dialog)

        wave_box = QComboBox()
        for label in ("正弦", "方波", "锯齿", "三角", "噪声"):
            wave_box.addItem(label)
        wave_box.setCurrentIndex(max(0, min(int(wave), 4)))
        form.addRow("波形", wave_box)

        boxes: dict[str, QDoubleSpinBox] = {}
        for key, label, _default, low, high, step, decimals in _AUDIO_SYNTH_FIELDS:
            box = QDoubleSpinBox()
            box.setRange(low, high)
            box.setSingleStep(step)
            box.setDecimals(decimals)
            box.setValue(rest[len(boxes)])
            form.addRow(label, box)
            boxes[key] = box

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        form.addRow(buttons)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return

        values = [boxes[key].value() for key, *_ in _AUDIO_SYNTH_FIELDS]
        self._audio_synth = (wave_box.currentIndex(), *values)
        self._send_value("wave", float(wave_box.currentIndex()))
        self._send_value("freq", values[0], values[1])
        self._send_value("length", values[2])
        self._send_value("envelope", values[3], values[4], values[5], values[6])
        self._send_value("cutoff", values[7])

    def _audio_scene_path(self) -> str:
        """上次用的那个场景文件。空字符串就是还没有过。"""
        return str(self._settings.value("audio/scene", ""))

    def _remember_audio_scene(self, path: str) -> None:
        self._settings.setValue("audio/scene", path)

    def _audio_folder(self) -> str:
        path = self._audio_scene_path()
        return str(Path(path).parent) if path else str(self._package)

    def _open_audio_scene(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "打开一个场景", self._audio_folder(), "场景 (*.scene);;所有文件 (*)")
        if not path:
            return
        self._remember_audio_scene(path)
        self._runner.write(f"L {path}\n")
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _import_wav(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "导入一个 WAV", self._audio_folder(), "WAV (*.wav);;所有文件 (*)")
        if not path:
            return
        self._runner.write(f"I {path}\n")
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _import_song(self) -> None:
        """读一份 `.song`（一首曲子）放进世界。进来之后在左边选中它，下面那块**音符格子**就能编了。"""
        path, _ = QFileDialog.getOpenFileName(
            self, "导入一首曲子", self._audio_folder(), "曲子 (*.song);;所有文件 (*)")
        if not path:
            return
        self._remember_audio_song(path)   # 「保存曲子」默认写回它来的那一份
        self._runner.write(f"U {path}\n")
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _save_song(self) -> None:
        """把格子上改的这一份写成 `.song`。没存过就先问一个地方，之后就一直是那儿了。"""
        path = self._audio_song_path()
        if not path:
            path, _ = QFileDialog.getSaveFileName(
                self, "曲子存到哪儿", self._audio_folder(), "曲子 (*.song)")
            if not path:
                return
            self._remember_audio_song(path)
        self._runner.write(f"B {path}\n")
        self._audio_timeline.setFocus(Qt.FocusReason.OtherFocusReason)

    def _audio_song_path(self) -> str:
        """上次那份曲子住在哪儿。空字符串就是还没有过。"""
        return str(self._settings.value("audio/song", ""))

    def _remember_audio_song(self, path: str) -> None:
        self._settings.setValue("audio/song", path)

    def _open_instruments(self) -> None:
        """开乐器编辑器。

        **模态**（`exec()`），但它自己那个嵌套事件循环里，帧和推表照常进来 —— 所以左边那份列表和右边
        那些参数一直跟着宿主走（见 `_apply_timeline`）。试听是循环的，所以**关掉的时候一定要说一声**
        （`InstrumentDialog.accept` / `closeEvent` 里已经做了）。
        """
        if self._instrument_dialog is not None:
            return
        dialog = InstrumentDialog(self)
        dialog.command.connect(self._on_timeline_command)
        dialog.set_instruments(self._timeline_instruments)
        self._instrument_dialog = dialog
        try:
            dialog.exec()
        finally:
            self._instrument_dialog = None
            dialog.deleteLater()

    def _close_instruments(self) -> None:
        """宿主那边没曲子了（或者停了）：把编辑器收起来。

        `reject()` 让 `exec()` 返回，接着 `_open_instruments` 把引用清掉 —— 这里**不**自己清，免得
        和那条路打架。
        """
        dialog = self._instrument_dialog
        if dialog is not None:
            dialog.reject()

    def _save_audio_scene(self) -> None:
        path = self._audio_scene_path()
        if not path:
            path, _ = QFileDialog.getSaveFileName(
                self, "存到哪儿", self._audio_folder(), "场景 (*.scene)")
            if not path:
                return
            self._remember_audio_scene(path)
        self._runner.write(f"S {path}\n")
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _export_wav(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "导出 WAV", self._audio_folder(), "WAV (*.wav)")
        if not path:
            return
        self._runner.write(f"E {path}\n")
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _on_audio_scene(self, line: str) -> None:
        """音频器把**声源表**发回来了（和建模器同一个哨兵前缀）—— 重建那个列表。

        行是 `begin` / `obj <号> <名字>` / `sel <号>` / `end`，和建模器一套格式；**另外多三种**：
        活动声源是合成音效时一行 `synth …`（给对话框填初值），是一首曲子时一行 `song …` 加一串
        `note …`（给音符格子画），以及（说了 `R 1` 之后）`play <拍>`。它们都在同一对 `begin`/`end`
        里 —— 一次推一整套，重画就完了，不用增量。
        """
        body = line[len(SCENE_PREFIX):].strip()
        if body == "begin":
            self._audio_sources = []
            self._timeline_notes = []
            self._timeline_instruments = []
            self._timeline_has_song = False
        elif body == "end":
            self._audio_syncing = True
            self._audio_objects.clear()
            for index, name in self._audio_sources:
                self._audio_objects.addItem(f"{index}   {name}")
            self._audio_objects.setCurrentRow(self._audio_active)
            self._audio_syncing = False
            self._apply_timeline()
        else:
            word, _, rest = body.partition(" ")
            if word == "obj":
                index, _, name = rest.partition(" ")
                self._audio_sources.append((int(index), name))
            elif word == "sel":
                self._audio_active = int(rest)
            elif word == "synth":
                # 活动声源是合成音效时，程序把它的配方回推过来：`synth <波形> <f0> <f1> <秒> <起音>
                # <衰减> <持续> <释音> <低通>`，固定顺序。面板拿它填那个对话框的初值。
                numbers = rest.split()
                if len(numbers) >= 9:
                    values = [float(number) for number in numbers[:9]]
                    self._audio_synth = (int(values[0]), *values[1:])
            elif word == "song":
                # `song <速度> <一遍拍数> <乐器数>` —— 后面跟着的就是这一首的音符。
                numbers = rest.split()
                if len(numbers) >= 3:
                    self._timeline_song = (float(numbers[0]), float(numbers[1]), int(numbers[2]))
                    self._timeline_has_song = True
            elif word == "note":
                # `note <起点> <长度> <音高> <力度> <乐器>` —— 顺序就是序号。
                numbers = rest.split()
                if len(numbers) >= 5:
                    self._timeline_notes.append((float(numbers[0]), float(numbers[1]),
                                                 int(numbers[2]), float(numbers[3]),
                                                 int(numbers[4])))
            elif word == "instrument":
                # `instrument <序号> <波形> <起音> <衰减> <持续> <释音> <增益> <低通> <种子>` ——
                # 顺序就是序号，所以存的是除序号之外的那八个。
                numbers = rest.split()
                if len(numbers) >= 9:
                    self._timeline_instruments.append((
                        int(float(numbers[1])), float(numbers[2]), float(numbers[3]),
                        float(numbers[4]), float(numbers[5]), float(numbers[6]),
                        float(numbers[7]), int(float(numbers[8]))))
            elif word == "play":
                try:
                    self._audio_timeline.set_playhead(float(rest))
                except ValueError:
                    pass

    def _apply_timeline(self) -> None:
        """一次推完之后：有曲子就把格子亮出来，没有就收起来。

        **播放头的上报也跟着这个开关走** —— 只在看得见格子的时候说 `R 1`，不然程序白报一路。
        """
        if self._timeline_has_song:
            tempo, loop, instruments = self._timeline_song
            self._audio_timeline.set_song(tempo, loop, instruments, self._timeline_notes)
            # 只在**用户没在编它们**的时候回填，否则一边打字一边被推回来的值顶掉。
            if not self._timeline_tempo.hasFocus():
                self._timeline_tempo.setValue(tempo)
            if not self._timeline_loop.hasFocus():
                self._timeline_loop.setValue(loop)
            self._timeline_hint.setText(f"{len(self._timeline_notes)} 个音符")
            self._timeline_instruments_button.setEnabled(True)
            # 乐器编辑器开着的话，让它跟着宿主这份走（列表和参数）。它正在编的那个框自己会让着。
            if self._instrument_dialog is not None:
                self._instrument_dialog.set_instruments(self._timeline_instruments)
            # **用 `isHidden()` 而不是 `isVisible()`**：切到别的页时整条这一页都是"不可见"的，
            # 那时候 `isVisible()` 永远是假，就会一直重复 `setVisible`、而藏的时候又藏不掉。
            if self._audio_timeline_panel.isHidden():
                self._audio_timeline_panel.setVisible(True)
            self._set_report_play(True)
        else:
            if not self._audio_timeline_panel.isHidden():
                self._audio_timeline_panel.setVisible(False)
                self._audio_timeline.clear()
            self._timeline_instruments_button.setEnabled(False)
            self._close_instruments()   # 没曲子了，乐器编辑器就没有来源了
            self._set_report_play(False)

    def _set_report_play(self, on: bool) -> None:
        """要不要程序每帧报播放位置（`R <0|1>`）。**只在变了的时候说一次**，不然每推一次表就发一行。"""
        if on == self._reporting_play:
            return
        self._reporting_play = on
        self._runner.write(f"R {1 if on else 0}\n")

    def _on_timeline_command(self, name: str, numbers: list) -> None:
        """音符格子动了：转成一行 `V <名字> <数…>`。

        **不把焦点还给 3D 画面**（`_send_value` 会还）—— 焦点得留在格子上，`Delete` 才删得掉，
        接着拖下一个音符也才连得上。
        """
        parts = " ".join(f"{number:g}" for number in numbers)
        self._runner.write(f"V {name} {parts}".rstrip() + "\n")

    def _on_tempo_edited(self) -> None:
        self._send_value("tempo", self._timeline_tempo.value())

    def _on_loop_edited(self) -> None:
        self._send_value("songloop", self._timeline_loop.value())

    def _on_audio_row(self, row: int) -> None:
        """大纲里点了一行：告诉音频器把第 row 个声源选成活动声源。"""
        if row < 0 or self._audio_syncing:
            return
        self._runner.write(f"N {row}\n")
        self._audio_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _audio_tools_enabled(self, on: bool) -> None:
        for button in self._audio_tools:
            button.setEnabled(on)

    def _on_model_tool(self, key: int) -> None:
        """工具条上按了一下：替用户敲一下那个键，然后**把焦点还给画面**。

        不还的话，下一个键就跑进编辑器而不是程序里 —— 按了一下「撤销」再想撤销一次，第二次就
        落空了。
        """
        self._runner.write(f"K {key} 1\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _on_model_primitive(self, kind: int) -> None:
        """图元那一排按了一下：**手里拿上一个形状**，下一次在画面上左键就把它放下。

        发的是 `P <图元号>`，不是一次按键 —— 形状是个值，敲键带不走它（见 `_MODEL_PRIMITIVES`）。
        发完照样把焦点还给画面，不然紧接着的那次左键落在编辑器上，形状就放不下去。
        """
        self._runner.write(f"P {kind}\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _on_scene(self, line: str) -> None:
        """子进程发回来的一张结构表（对象 / 声源）—— **按现在跑的是谁**分流到大纲。

        两个工具用的是同一个哨兵前缀和同一套行（`begin` / `obj` / `sel` / `end`），所以分流只要看
        `_audio_running`：跑的是音频器就填声源列表，否则填对象列表。
        """
        if self._audio_running:
            self._on_audio_scene(line)
        else:
            self._on_model_scene(line)

    def _on_model_scene(self, line: str) -> None:
        """建模器把**对象表**发回来了（带哨兵前缀，见 `runtime.SCENE_PREFIX`）—— 重建那个列表。

        行是 `begin` / `obj <号> <名字>` / `sel <号>` / `end`。名字里可以有空格，所以取的是"号之后
        的全部"。重建时把 `_model_syncing` 立起来，免得 `setCurrentRow` 又发一条 `N` 回去。
        """
        body = line[len(SCENE_PREFIX):].strip()
        if body == "begin":
            self._model_scene = []
        elif body == "end":
            self._model_syncing = True
            self._model_objects.clear()
            for index, name in self._model_scene:
                self._model_objects.addItem(f"{index}   {name}")
            self._model_objects.setCurrentRow(self._model_active)
            self._model_syncing = False
        else:
            word, _, rest = body.partition(" ")
            if word == "obj":
                index, _, name = rest.partition(" ")
                self._model_scene.append((int(index), name))
            elif word == "sel":
                self._model_active = int(rest)

    def _on_model_row(self, row: int) -> None:
        """大纲里点了一行：告诉建模器把第 row 个对象选成活动对象。"""
        if row < 0 or self._model_syncing:
            return
        self._runner.write(f"N {row}\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _model_tools_enabled(self, on: bool) -> None:
        for button in self._model_tools:
            button.setEnabled(on)

    # -- 模型的存和读 ------------------------------------------------------------------------
    #
    # 这两颗按钮走的是另一条路：**路径是字符串**，键盘那套送不了它。`editor_host.h` 为这种事留了
    # 两行 —— `S <路径>` 和 `L <路径>`，路径是整行剩下的全部（不分词，路径里有空格）。
    #
    # 为什么不敲键盘：Windows 把命令行按 ANSI 代码页交给 C 运行库，`--model 中文路径` 还没进
    # `main` 就已经不是原来那串字节了。stdin 上这串是 UTF-8 的字节，原样到得了。

    def _colour_model(self) -> None:
        """给选中的那个面挑一个颜色。

        颜色是**四个 0..1 的数**发下去的（不是 Qt 那个 `QColor`）—— 管子里没有对象，只有数。
        """
        colour = QColorDialog.getColor(QColor(200, 205, 215), self, "给选中的东西挑一个颜色")
        if not colour.isValid():
            return
        self._runner.write(f"A {colour.redF():.6f} {colour.greenF():.6f} "
                           f"{colour.blueF():.6f} {colour.alphaF():.6f}\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _texture_model(self) -> None:
        """给选中的面挂一张图。

        只有 **BMP**（不压缩的那些）—— 别的格式要解码器，而一个 inflate 不是能顺手写的。
        """
        path, _ = QFileDialog.getOpenFileName(
            self, "挑一张图", self._model_folder(), "BMP 图 (*.bmp);;所有文件 (*)")
        if not path:
            return
        self._runner.write(f"T {path}\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _insert_picture(self) -> None:
        """往场景里放一张图（BMP），落在画面中央。"""
        path, _ = QFileDialog.getOpenFileName(
            self, "挑一张图", self._model_folder(), "BMP 图 (*.bmp);;所有文件 (*)")
        if not path:
            return
        self._runner.write(f"J {path}\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _export_scene(self) -> None:
        """把整个场景导出成一个 .scene（游戏直接读）。"""
        path, _ = QFileDialog.getSaveFileName(
            self, "导出场景", self._model_folder(), "场景 (*.scene)")
        if not path:
            return
        self._runner.write(f"E {path}\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _open_model(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "打开一个模型", self._model_folder(), "模型 (*.model);;所有文件 (*)")
        if not path:
            return
        self._remember_model(path)
        self._runner.write(f"L {path}\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _import_model(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "导入一个网格文件", self._model_folder(), "OBJ (*.obj);;所有文件 (*)")
        if not path:
            return
        # **不记成"手上这份的路径"** —— 那是别人的文件，按「保存」不该往它上面写。导完之后
        # 「保存」会先问存到哪儿，那正好是"我的模型"和"借来的文件"该有的区别。
        self._runner.write(f"I {path}\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _save_model(self) -> None:
        path = self._model_path()
        if not path:
            # 还没存过：先问一个地方。存过一次之后就一直是那个地方了。
            path, _ = QFileDialog.getSaveFileName(
                self, "存到哪儿", self._model_folder(), "模型 (*.model)")
            if not path:
                return
            self._remember_model(path)
        self._runner.write(f"S {path}\n")
        self._model_view.setFocus(Qt.FocusReason.OtherFocusReason)

    def _model_path(self) -> str:
        """上次用的那个模型文件。空字符串就是还没有过。"""
        return str(self._settings.value("model/path", ""))

    def _remember_model(self, path: str) -> None:
        self._settings.setValue("model/path", path)

    def _model_folder(self) -> str:
        """文件对话框从哪儿开：上次那个文件待的地方，还没用过就是包根。"""
        path = self._model_path()
        return str(Path(path).parent) if path else str(self._package)

    def _active_view(self) -> Viewport:
        """这一轮运行画在哪块面板上。

        **按跑的是什么分，不按在看哪一页** —— 建模器跑到代码那一页的面板上，看起来就像刚跑了
        个游戏。
        """
        if self._audio_running:
            return self._audio_view
        return self._model_view if self._model_running else self._viewport

    def _build_menus(self) -> None:
        file_menu = self.menuBar().addMenu("文件")
        file_menu.addAction(self.act_new)
        new_from = file_menu.addMenu("从模板新建")
        for action in self.act_templates:
            new_from.addAction(action)
        file_menu.addAction(self.act_open)
        file_menu.addAction(self.act_open_folder)
        file_menu.addSeparator()
        file_menu.addAction(self.act_save)
        file_menu.addAction(self.act_save_as)
        file_menu.addSeparator()
        file_menu.addAction(self.act_close_tab)
        file_menu.addAction(self.act_quit)

        edit_menu = self.menuBar().addMenu("编辑")
        for action in (self.act_undo, self.act_redo):
            edit_menu.addAction(action)
        edit_menu.addSeparator()
        for action in (self.act_cut, self.act_copy, self.act_paste):
            edit_menu.addAction(action)
        edit_menu.addSeparator()
        for action in (self.act_find, self.act_replace, self.act_find_next,
                       self.act_find_previous, self.act_goto):
            edit_menu.addAction(action)
        edit_menu.addSeparator()
        for action in (self.act_comment, self.act_duplicate, self.act_move_up, self.act_move_down):
            edit_menu.addAction(action)
        edit_menu.addSeparator()
        for action in (self.act_zoom_in, self.act_zoom_out, self.act_zoom_reset):
            edit_menu.addAction(action)

        run_menu = self.menuBar().addMenu("运行")
        run_menu.addAction(self.act_run)
        run_menu.addAction(self.act_stop)
        run_menu.addSeparator()
        run_menu.addAction(self.act_pack)
        run_menu.addSeparator()
        run_menu.addAction(self.act_clear)

        bar = self.addToolBar("主")
        bar.setMovable(False)
        bar.setObjectName("main")
        bar.addAction(self.act_page_code)
        bar.addAction(self.act_page_model)
        bar.addAction(self.act_page_audio)
        bar.addSeparator()
        for action in (self.act_new, self.act_open, self.act_save):
            bar.addAction(action)
        bar.addSeparator()
        bar.addAction(self.act_run)
        bar.addAction(self.act_stop)
        bar.addAction(self.act_pack)
        bar.addSeparator()
        bar.addWidget(QLabel("语言 "))
        self._language = QComboBox()
        for label, value, _suffix in _CHOICES:
            self._language.addItem(label, value)
        self._language.setToolTip("这个文件按哪种语言处理：怎么上色、注释用什么、运行走哪条路")
        self._language.currentIndexChanged.connect(self._on_language_chosen)
        bar.addWidget(self._language)
        bar.addWidget(QLabel("参数 "))
        self._arguments = QLineEdit()
        self._arguments.setPlaceholderText("传给脚本的命令行参数")
        self._arguments.setMaximumWidth(280)
        bar.addWidget(self._arguments)

    # -- documents -------------------------------------------------------------------------

    def _new_file(self, name: str = "untitled.py", text: str = "") -> CodeEditor:
        language = _LANGUAGES.get(Path(name).suffix.lower(), "text")
        editor = CodeEditor(language=language)
        editor.setPlainText(text)
        editor.set_font_point_size(self._font_size)
        editor.setProperty("display", name)
        editor.cursorPositionChanged.connect(self._update_position)
        editor.document().modificationChanged.connect(
            lambda _changed, made=editor: self._refresh_tab_title(made))
        editor.document().contentsChanged.connect(
            lambda made=editor: self._on_contents_changed(made))

        index = self._tabs.addTab(editor, name)
        self._refresh_tab_title(editor)
        self._tabs.setCurrentIndex(index)
        self._sync_language()
        self._update_actions()
        return editor

    def _current_editor(self) -> CodeEditor | None:
        widget = self._tabs.currentWidget()
        return widget if isinstance(widget, CodeEditor) else None

    def _on_editor(self, method: str, *args) -> None:
        editor = self._current_editor()
        if editor is not None:
            getattr(editor, method)(*args)

    def _path_of(self, editor: CodeEditor) -> Path | None:
        stored = editor.property("path")
        return Path(stored) if stored else None

    def _display_name(self, editor: CodeEditor) -> str:
        path = self._path_of(editor)
        return path.name if path is not None else str(editor.property("display") or "untitled.py")

    def _refresh_tab_title(self, editor: CodeEditor) -> None:
        index = self._tabs.indexOf(editor)
        if index < 0:
            return
        mark = "• " if editor.document().isModified() else ""
        self._tabs.setTabText(index, mark + self._display_name(editor))
        self._tabs.setTabToolTip(index, str(self._path_of(editor) or "未保存"))

    def _open_path(self, path: Path) -> None:
        for index in range(self._tabs.count()):
            editor = self._tabs.widget(index)
            if isinstance(editor, CodeEditor) and self._path_of(editor) == path:
                self._tabs.setCurrentIndex(index)
                return
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as problem:
            QMessageBox.warning(self, "打不开", f"{path}\n{problem}")
            return

        editor = self._new_file(name=path.name, text=text)
        editor.setProperty("path", str(path))
        editor.document().setModified(False)
        self._refresh_tab_title(editor)

    def _open_file(self) -> None:
        chosen, _ = QFileDialog.getOpenFileName(
            self, "打开", str(self._root), "Python (*.py);;所有文件 (*)"
        )
        if chosen:
            self._open_path(Path(chosen))

    def _open_folder(self) -> None:
        chosen = QFileDialog.getExistingDirectory(self, "打开文件夹", str(self._root))
        if chosen:
            self._root = Path(chosen)
            self._tree_model.setRootPath(chosen)
            self._tree.setRootIndex(self._tree_model.index(chosen))

    def _on_tree_activated(self, index: QModelIndex) -> None:
        path = Path(self._tree_model.filePath(index))
        if path.is_file():
            self._open_path(path)

    def _save(self) -> bool:
        editor = self._current_editor()
        if editor is None:
            return False
        path = self._path_of(editor)
        return self._save_as() if path is None else self._write(editor, path)

    def _save_as(self) -> bool:
        editor = self._current_editor()
        if editor is None:
            return False
        suggested = self._path_of(editor) or (self._root / "游戏.py")
        chosen, _ = QFileDialog.getSaveFileName(
            self, "另存为", str(suggested), "Python (*.py);;所有文件 (*)"
        )
        return bool(chosen) and self._write(editor, Path(chosen))

    def _write(self, editor: CodeEditor, path: Path) -> bool:
        try:
            path.write_text(editor.toPlainText(), encoding="utf-8")
        except OSError as problem:
            QMessageBox.warning(self, "存不了", f"{path}\n{problem}")
            return False
        editor.setProperty("path", str(path))
        editor.setProperty("display", path.name)
        editor.document().setModified(False)
        self._refresh_tab_title(editor)
        self.statusBar().showMessage(f"已保存 {path}", 3000)
        return True

    def _close_tab(self, index: int) -> None:
        if index < 0:
            return
        editor = self._tabs.widget(index)
        if isinstance(editor, CodeEditor) and editor.document().isModified():
            answer = QMessageBox.question(
                self,
                "未保存",
                f"{self._tabs.tabText(index)} 有未保存的改动，关闭吗？",
                QMessageBox.StandardButton.Discard | QMessageBox.StandardButton.Cancel,
            )
            if answer != QMessageBox.StandardButton.Discard:
                return
        self._tabs.removeTab(index)
        if self._tabs.count() == 0:
            self._new_file()
        self._update_actions()

    def _on_tab_changed(self, _index: int) -> None:
        self._sync_language()
        if self._findbar.isVisible():
            editor = self._current_editor()
            if editor is not None:
                self._findbar.open_on(editor, self._findbar.replacing)
        self._update_actions()
        self._update_position()

    # -- which language the file is --------------------------------------------------------

    def _new_suffix(self) -> str:
        """The suffix a new file gets, so New makes what the dropdown is showing."""
        index = max(0, self._language.currentIndex())
        return _CHOICES[index][2]

    def _sync_language(self) -> None:
        """Point the dropdown at what the current tab actually is."""
        editor = self._current_editor()
        if editor is None:
            self._language.setEnabled(False)
            return
        self._language.blockSignals(True)
        self._language.setCurrentIndex(max(0, self._language.findData(editor.language)))
        self._language.blockSignals(False)
        self._language.setEnabled(True)

    def _on_language_chosen(self, index: int) -> None:
        editor = self._current_editor()
        if editor is None:
            return
        editor.set_language(self._language.itemData(index))
        self.statusBar().showMessage(
            f"{self._display_name(editor)} 按 {self._language.itemText(index)} 处理", 3000)

    def _on_contents_changed(self, editor: CodeEditor) -> None:
        # Keep the hits in step while typing, but only for the file the bar is looking at.
        if self._findbar.isVisible() and self._findbar.target is editor:
            self._findbar.refresh()

    # -- finding ---------------------------------------------------------------------------

    def _open_find(self, replace: bool) -> None:
        editor = self._current_editor()
        if editor is not None:
            self._findbar.open_on(editor, replace)

    def _find(self, backward: bool) -> None:
        if self._findbar.isVisible():
            self._findbar.find_next(backward)
        else:
            self._open_find(False)

    def _on_matches(self, spans: list[tuple[int, int]]) -> None:
        editor = self._current_editor()
        if editor is not None:
            editor.set_search_spans(spans)

    def _on_find_closed(self) -> None:
        editor = self._current_editor()
        if editor is not None:
            editor.set_search_spans([])

    def _goto_line(self) -> None:
        editor = self._current_editor()
        if editor is None:
            return
        line, ok = QInputDialog.getInt(self, "转到行", "行号", editor.textCursor().blockNumber() + 1,
                                       1, max(1, editor.blockCount()))
        if ok:
            editor.goto_line(line)

    def _zoom(self, direction: int) -> None:
        self._font_size = 11 if direction == 0 else max(6, min(48, self._font_size + direction))
        for index in range(self._tabs.count()):
            editor = self._tabs.widget(index)
            if isinstance(editor, CodeEditor):
                editor.set_font_point_size(self._font_size)

    # -- running ---------------------------------------------------------------------------

    def _run(self) -> None:
        if self._runner.running():
            return
        editor = self._current_editor()
        if editor is None:
            return

        # F5 跑的是手上这个文件，不是建模器/音频器 —— 于是画面回到代码那一页的面板上。**在这里说
        # 一次就够了**：`_run` 是所有 Run 的入口，`_build_then_run` 也是从这儿来的。
        self._model_running = False
        self._audio_running = False

        target = self._path_of(editor)
        if target is None or editor.document().isModified():
            # Run what is on screen, not what was last saved.
            if not self._save():
                return
            target = self._path_of(editor)
        if target is None:
            return

        try:
            arguments = split_arguments(self._arguments.text())
        except ValueError as problem:
            QMessageBox.warning(self, "参数读不了", str(problem))
            return

        if editor.language == "cpp":
            self._build_then_run(target, arguments)
        else:
            self._start(f"$ {target} {' '.join(arguments)}".rstrip(), _RUNNING)
            self._runner.run(python_command(target, arguments))

    def _start(self, headline: str, label: str) -> None:
        self._console.append_line("")
        self._console.append_line(headline)
        # 擦的是**这一轮要画的那一块**：从建模页按 F5 跑一个脚本，擦的不该是建模那块。
        self._active_view().clear()
        self._frame_label.setText("")
        self._model_frame_label.setText("")
        self._audio_frame_label.setText("")
        self._mirror.stop()
        self.act_run.setEnabled(False)
        self.act_stop.setEnabled(True)
        self._run_label.setText(label)

    def _idle(self) -> None:
        self._mirror.stop()
        self._viewport.set_capturing(False)
        self._model_view.set_capturing(False)
        self._audio_view.set_capturing(False)
        self._model_tools_enabled(False)
        self._audio_tools_enabled(False)
        self._model_running = False
        self._audio_running = False
        # 曲子那块也收起来：跑着的东西没了，它就没有来源了。
        self._reporting_play = False
        self._timeline_has_song = False
        self._audio_timeline.clear()
        self._audio_timeline_panel.setVisible(False)
        self._close_instruments()
        self.act_run.setEnabled(self._current_editor() is not None)
        self.act_pack.setEnabled(self._current_editor() is not None)
        self.act_stop.setEnabled(False)

    def _stop(self) -> None:
        self._mirror.stop()
        self._runner.stop()
        self._run_label.setText("已停止")
        self._idle()

    def _on_output(self, line: str) -> None:
        self._console.append_line(line)

    def _on_frame(self, rgba: bytes, pitch: int, width: int, height: int) -> None:
        if self._audio_running:
            self._audio_view.show_frame(rgba, pitch, width, height)
            self._audio_frame_label.setText(
                f"{width}×{height} · 第 {self._audio_view.frames} 帧")
            return
        if self._model_running:
            self._model_view.show_frame(rgba, pitch, width, height)
            self._model_frame_label.setText(
                f"{width}×{height} · 第 {self._model_view.frames} 帧")
            return
        self._viewport.show_frame(rgba, pitch, width, height)
        self._frame_label.setText(f"{width}×{height} · 第 {self._viewport.frames} 帧")

    def _on_mirrored(self, picture) -> None:
        self._viewport.show_image(picture)
        self._frame_label.setText(
            f"{picture.width()}×{picture.height()} · 程序自己的窗口 · 第 {self._viewport.frames} 帧")

    # What the panel collects, on its way to the program. **These six are one protocol**, written
    # down the child's stdin a line at a time; the other end is `宿主/editor_host.h`'s `consume`.
    # Change one and you have changed the other -- there is a copy of this table there.
    #
    #     K <虚拟键码> <1 按下 / 0 抬起> [修饰键]   修饰键 1 Shift / 2 Ctrl / 4 Alt
    #     M <x> <y>                        指针，**画面自己的像素**（面板把缩放和居中算掉了）
    #     C <1|2|4> <1 按下 / 0 抬起> <x> <y>   左右中键，**带上它发生在哪**
    #     W <格数>                         滚轮，带符号
    #     A <R> <G> <B> <A>               给选中的面上这个颜色，四个通道 0..1
    #     S <路径>                         存到这个路径
    #     L <路径>                         从这个路径读自己的模型
    #     I <路径>                         从这个路径导入别人的网格文件
    #     T <路径>                         给选中的面挂上这个路径的一张图
    #     J <路径>                         往场景里放一张图（一块贴着它的平面对象）
    #     E <路径>                         把整个场景导出到这个 .scene
    #     P <图元号>                        手里拿上一个图元（1 方块 / 2 网格 / 3 圆盘 / 4 圆柱 /
    #                                       5 圆锥 / 6 球 / 7 圆环 / 8 金字塔 / 9 圆台），
    #                                       下一次左键就把它放在画面上
    #     V <名字> <数…>                    把某个参数设成这个值（音频工具：gain / reference /
    #                                       max / master / loop / mute / freq / length）
    #
    # **`L` / `I` / `E` / `P` 是"一个字母、两套含义"**：建模器把它们读成"读自己的模型 / 导入 OBJ /
    # 导出场景 / 放一个图元"，音频器把它们读成"读一个场景 / 导入 WAV / 导出 WAV / 放一段声源"。
    # 两个工具从不同时跑，面板发的话是给**正在跑的那一个**的，所以一个字母够用 —— 而每个工具认不出
    # 的行丢掉，所以将来多一种消息不会让旧的程序炸掉。
    #
    # 带路径的那几条是**整行剩下的全部**，不分词 —— 路径里有空格，按空白切会把它切成两半。它们
    # 也不走键盘那条路：见 `_open_model` 上面那段，路径只能从管子里送。`P` 带的是一个**号**，和
    # 上色 `A` 一样属于"值"那一类，不走键盘。
    #
    # 按键那一行带位置，是因为"指针现在在哪"和"这一下按在哪"不是一回事：两者可能落在同一帧里，
    # 去问指针会拿到移动之后的值。落在画面外面的按下没有画面坐标可给，那就只写前两个数，程序
    # 自己回落到指针 —— 但**还是要发**，否则拖出画面再松手，程序就永远以为键还按着。

    def _on_view_key(self, virtual_key: int, down: bool, modifiers: int) -> None:
        """A key typed in the panel, on its way to the program as the code a window would give it."""
        self._runner.write(f"K {virtual_key} {1 if down else 0} {modifiers}\n")

    def _on_view_moved(self, x: int, y: int) -> None:
        self._runner.write(f"M {x} {y}\n")

    def _on_view_clicked(self, button: int, down: bool, x: int, y: int) -> None:
        state = 1 if down else 0
        if x < 0 or y < 0:
            self._runner.write(f"C {button} {state}\n")
        else:
            self._runner.write(f"C {button} {state} {x} {y}\n")

    def _on_view_wheeled(self, notches: int) -> None:
        self._runner.write(f"W {notches}\n")

    def _on_finished(self, code: int, seconds: float) -> None:
        if self._packing is not None:
            plan = self._packing
            self._packing = None
            if code != 0:
                self._console.append_line(f"[打包没成功，退出码 {code} · {seconds:.1f}s]")
                self._run_label.setText(f"打包失败 · {seconds:.1f}s")
                self._idle()
                return
            self._console.append_line(f"[编好了 · {seconds:.1f}s]")
            self._run_label.setText(f"打好了 · {seconds:.1f}s")
            self._finish_packing(plan)
            return

        if self._after_build is not None:
            program, working_directory = self._after_build
            self._after_build = None
            if code != 0:
                self._console.append_line(f"[编译没通过，退出码 {code}]")
                self._run_label.setText(f"编译失败 · {seconds:.2f}s")
                self._idle()
                return
            self._console.append_line(f"[编好了，{seconds:.2f}s]")
            # 后端整个在一个 DLL 里，而它得在**程序自己那一层**才找得到 —— 见 `bring_beside`。
            if self._project is not None:
                for binary in self._project.bring_beside(program):
                    self._console.append_line(f"[放在旁边] {binary.name}")
            self._launch(program, working_directory)
            return

        verdict = "退出码 0" if code == 0 else f"退出码 {code}"
        self._run_label.setText(f"{verdict} · {seconds:.2f}s")
        self._console.append_line(f"[{verdict}, {seconds:.2f}s]")
        self._idle()

    # -- C++ ---------------------------------------------------------------------------------

    def _build_then_run(self, source: Path, arguments: list[str]) -> None:
        toolchain = self._find_toolchain()
        if not toolchain.ok:
            self._console.append_line("")
            self._console.append_line("[用不了 C++]")
            # One line per line: `problem` says which path it looked at, and a path a reader cannot
            # see is the difference between "the package is broken" and "the exe was moved".
            for line in toolchain.problem.splitlines():
                self._console.append_line(f"  {line}")
            return
        project = self._find_project()
        if project is None:
            self._console.append_line("")
            self._console.append_line("[没有项目清单]")
            self._console.append_line(f"  找的是 {self._package / cpp.MANIFEST}")
            return
        if not self._kernel_ready(project):
            return
        refusal = project.refusal(source)
        if refusal:
            self._console.append_line("")
            self._console.append_line(f"[{refusal}]")
            return

        target = project.target_for(source)
        sources = project.sources_for(source)
        output = project.output_directory(source)
        program = project.program_for(source)
        if target is None:
            # A program someone is writing is the whole folder (see `sources_for`), so say how many
            # files went in -- seeing "1" when you expected your four is the whole diagnosis.
            beside = "" if len(sources) == 1 else f" + 同目录 {len(sources) - 1} 个"
            self._start(f"$ zig c++ {source.name}{beside}", _BUILDING)
        else:
            self._start(f"$ {target.name} · {len(sources)} 个源文件", _BUILDING)

        if project.is_current(source):
            # Nothing changed since the last build, so there is nothing to watch compile.
            self._console.append_line("(源码和库都没变，用上次编好的)")
            if self._project is not None:
                self._project.bring_beside(program)
            self._launch(program, source.parent)
            return

        output.mkdir(parents=True, exist_ok=True)
        self._after_build = (program, source.parent)
        self._toolchain = toolchain
        self._runner.run(Command(program=str(toolchain.compiler),
                                 arguments=project.arguments(source, toolchain),
                                 working_directory=output,
                                 # clang writes its diagnostics in UTF-8, whatever the console's
                                 # codepage is.
                                 encoding="utf-8"))

    def _launch(self, program: Path, working_directory: Path) -> None:
        try:
            arguments = split_arguments(self._arguments.text())
        except ValueError as problem:
            QMessageBox.warning(self, "参数读不了", str(problem))
            return
        if not program.is_file():
            self._console.append_line(f"[没有 {program}]")
            self._idle()
            return
        self._run_label.setText(_RUNNING)
        self.act_run.setEnabled(False)
        self.act_stop.setEnabled(True)
        self._runner.run(program_command(program, arguments, working_directory))
        # A program launched with `ZL_EDITOR_HOST` presents into the panel and has no window of its
        # own, so the panel is where its keyboard is. A program that ignored the variable opened its
        # own window instead, and the mirror copies that; watching for it costs nothing.
        self._viewport.set_capturing(True)
        self._viewport.setFocus(Qt.FocusReason.OtherFocusReason)
        self._mirror.watch(self._runner.process_id())

    def _find_toolchain(self) -> "cpp.Toolchain":
        """The compiler, looked for again until one is found.

        Against `_package`, not `_root`: see where they are set. **A failure is not remembered** --
        it is one local file check, and remembering a failure means a window that said "no compiler"
        while the package was being rebuilt keeps saying it long after the compiler is back.
        """
        if self._toolchain is None or not self._toolchain.ok:
            self._toolchain = cpp.Toolchain.find(self._package)
        return self._toolchain

    def _find_project(self) -> "cpp.Project | None":
        """The manifest, same root and same rule as the compiler: a miss is re-checked."""
        if self._project is None:
            self._project = cpp.Project.discover(self._package)
        return self._project

    def _kernel_ready(self, project: "cpp.Project") -> bool:
        """内核编成 `.a` 了吗。没有就先说清楚，别让 Zig 去报。

        **没有 `.a` 就没法链接**，而 Zig 给出的是一串 `unable to find dynamic system library
        'zlong_system'` —— 那句话指向链接器，读的人不会知道真正缺的是"内核还没编过"。所以这里先
        拦一下，把该做什么说出来。

        **`build/` 里那些 MSVC 的 `.lib` 顶不上**：ABI 不一样（名字修饰、STL 布局都不同），
        Zig 链不了。所以"我用 Visual Studio 构建过了"并不等于"编辑器能编 C++"。
        """
        if project.zig_libraries:
            return True
        self._console.append_line("")
        self._console.append_line("[内核还没编出来 —— 一个 .a 都没找到]")
        self._console.append_line("  内核、引擎、宿主要先编成静态库，编辑器才链得上。跑一次：")
        self._console.append_line("      python 打包.py")
        self._console.append_line(
            "  （`build/` 里那些 MSVC 的 .lib 顶不上 —— ABI 不一样，Zig 链不了。）")
        return False

    # -- 打包 ---------------------------------------------------------------------------------

    def _pack_game(self) -> None:
        """把当前这个文件所在的文件夹打成一个能拿走的包。

        游戏就是这个文件夹 —— 和"一个文件夹就是一个程序"是同一条规矩，所以打包不需要另立一套
        "这个游戏有哪些文件"的说法。见 `packager.py`。
        """
        if self._runner.running():
            return
        editor = self._current_editor()
        if editor is None:
            return
        target = self._path_of(editor)
        if target is None or editor.document().isModified():
            if not self._save():
                return
            target = self._path_of(editor)
        if target is None:
            return

        plan = packager.plan_for(target, project=self._find_project())
        name, accepted = QInputDialog.getText(self, "打包游戏", "游戏名", text=plan.name)
        if not accepted or not name.strip():
            return
        plan = packager.plan_for(target, name=name.strip(), project=self._find_project())
        # 产物放在游戏文件夹**旁边**的 发行/ 里 —— 和 打包.py 在项目根上做的事一样。
        plan.output = plan.folder.parent / "发行" / plan.name

        unsafe = packager.refusal(plan)
        if unsafe:
            self._complain("[打包不了：产物会盖住游戏]", unsafe)
            return

        self._pack_cxx(plan)

    def _pack_cxx(self, plan: packager.Plan) -> None:
        toolchain = self._find_toolchain()
        if not toolchain.ok:
            self._complain("[打包不了：没有编译器]", toolchain.problem)
            return
        project = self._find_project()
        if project is None:
            self._console.append_line("")
            self._console.append_line("[打包不了：没有项目清单]")
            self._console.append_line(f"  找的是 {self._package / cpp.MANIFEST}")
            return
        if not plan.sources:
            self._complain(f"[{plan.folder} 里没有 .cpp]", "这个文件夹里没有能编的东西。")
            return
        if not self._kernel_ready(project):
            return
        if not self._agree_to_overwrite(plan.output):
            return

        self._console.append_line("")
        self._console.append_line(
            f"$ 打包 {plan.name} · C++ · {len(plan.sources)} 个源文件")
        self._begin_packing(plan)
        packager.clear(plan.output)
        self._runner.run(Command(program=str(toolchain.compiler),
                                 arguments=packager.cxx_arguments(plan, project, toolchain),
                                 working_directory=str(plan.output),
                                 encoding="utf-8"))

    def _begin_packing(self, plan: packager.Plan) -> None:
        self._packing = plan
        self.act_run.setEnabled(False)
        self.act_pack.setEnabled(False)
        self.act_stop.setEnabled(True)
        self._run_label.setText("打包中…")

    def _finish_packing(self, plan: packager.Plan) -> None:
        """子进程跑完了：清掉编译中间物，把数据文件拷过去，然后报账。"""
        plan.leftovers = packager.tidy(plan)
        copied = packager.copy_data(plan)
        self._console.append_line(f"[打好了 · 数据 {copied} 个文件]")
        for line in packager.report(plan):
            self._console.append_line(line)
        self._idle()

    def _complain(self, headline: str, detail: str) -> None:
        self._console.append_line("")
        self._console.append_line(headline)
        for line in detail.splitlines():
            self._console.append_line(f"  {line}")

    def _agree_to_overwrite(self, output: Path) -> bool:
        if not output.exists():
            return True
        answer = QMessageBox.question(
            self, "已经有了",
            f"{output}\n已经在了，覆盖掉吗？",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        return answer == QMessageBox.StandardButton.Yes

    def _jump_to(self, path_text: str, line: int) -> None:
        path = Path(path_text)
        if not path.is_file():
            self.statusBar().showMessage(f"找不到 {path}", 4000)
            return
        self._open_path(path)
        editor = self._current_editor()
        if editor is not None:
            editor.goto_line(line)

    # -- state -----------------------------------------------------------------------------

    def _update_actions(self) -> None:
        has = self._current_editor() is not None
        for action in (self.act_save, self.act_save_as, self.act_undo, self.act_redo, self.act_cut,
                       self.act_copy, self.act_paste, self.act_find, self.act_replace,
                       self.act_find_next, self.act_find_previous, self.act_goto, self.act_comment,
                       self.act_duplicate, self.act_move_up, self.act_move_down):
            action.setEnabled(has)
        self.act_run.setEnabled(has and not self._runner.running())
        self.act_pack.setEnabled(has and not self._runner.running())

    def _update_position(self) -> None:
        editor = self._current_editor()
        if editor is None:
            self._position.setText("")
            return
        cursor = editor.textCursor()
        self._position.setText(f"行 {cursor.blockNumber() + 1}  列 {cursor.columnNumber() + 1}")

    def _refresh_backend_note(self) -> None:
        backend = find_backend()
        if backend is None:
            self._backend_label.setText("后端未构建 — 运行前请先构建 zlong")
            self._backend_label.setStyleSheet("color:#e0a030")
        else:
            self._backend_label.setText(f"后端 {backend}")
            self._backend_label.setStyleSheet("color:#4ec9b0")

    def _restore_session(self) -> None:
        geometry = self._settings.value("window/geometry")
        if geometry is not None:
            self.restoreGeometry(geometry)
        state = self._settings.value("window/state")
        if state is not None:
            self.restoreState(state)

        self._font_size = int(self._settings.value("editor/font", 11))
        was_open = self._settings.value("session/files", [])
        for name in was_open if isinstance(was_open, list) else []:
            path = Path(name)
            if path.is_file():
                self._open_path(path)
        current = int(self._settings.value("session/current", 0))
        if self._tabs.count() == 0:
            self._new_file()
        else:
            self._tabs.setCurrentIndex(max(0, min(current, self._tabs.count() - 1)))

    def _save_session(self) -> None:
        self._settings.setValue("window/geometry", self.saveGeometry())
        self._settings.setValue("window/state", self.saveState())
        self._settings.setValue("editor/font", self._font_size)
        open_files = []
        for index in range(self._tabs.count()):
            editor = self._tabs.widget(index)
            path = self._path_of(editor) if isinstance(editor, CodeEditor) else None
            if path is not None:
                open_files.append(str(path))
        self._settings.setValue("session/files", open_files)
        self._settings.setValue("session/current", self._tabs.currentIndex())

    # -- events ----------------------------------------------------------------------------

    def closeEvent(self, event) -> None:  # noqa: N802 - Qt's name
        for index in range(self._tabs.count()):
            editor = self._tabs.widget(index)
            if isinstance(editor, CodeEditor) and editor.document().isModified():
                self._tabs.setCurrentIndex(index)
                if not self._confirm_quit():
                    event.ignore()
                    return
        self._runner.stop()
        self._mirror.stop()
        self._save_session()
        event.accept()

    def _confirm_quit(self) -> bool:
        name = self._tabs.tabText(self._tabs.currentIndex())
        answer = QMessageBox.question(
            self,
            "未保存",
            f"{name} 有未保存的改动，退出前保存吗？",
            QMessageBox.StandardButton.Save
            | QMessageBox.StandardButton.Discard
            | QMessageBox.StandardButton.Cancel,
        )
        if answer == QMessageBox.StandardButton.Save:
            return self._save()
        return answer == QMessageBox.StandardButton.Discard


def _dark_palette() -> QPalette:
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window, QColor("#1e1e1e"))
    palette.setColor(QPalette.ColorRole.WindowText, QColor("#d4d4d4"))
    palette.setColor(QPalette.ColorRole.Base, QColor("#1e1e1e"))
    palette.setColor(QPalette.ColorRole.Text, QColor("#d4d4d4"))
    palette.setColor(QPalette.ColorRole.Highlight, QColor("#094771"))
    palette.setColor(QPalette.ColorRole.HighlightedText, QColor("#ffffff"))
    return palette


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv if argv is None else argv)
    app = QApplication(argv)
    app.setOrganizationName("zhuLong")
    app.setApplicationName("编码器")
    app.setStyle("Fusion")
    app.setPalette(_dark_palette())
    # **窗口标题栏上那个图标要 Qt 自己设。** exe 自己那个是 PyInstaller 嵌进去的（`--icon`），
    # 任务栏和资源管理器看的是它；而标题栏上这个不设就是 Qt 的默认图标，两处对不上。
    icon = editor_asset("icon.ico")
    if icon.is_file():
        app.setWindowIcon(QIcon(str(icon)))

    window = EditorWindow()
    for argument in argv[1:]:
        path = Path(argument)
        if path.is_file():
            window._open_path(path)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
