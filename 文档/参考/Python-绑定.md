# Python 绑定 — `import zlong` 拿到的全部

这一份是 `文档/提取绑定.py` 从**编出来的模块**自省出来的：签名是问 pybind11 要的，不是解出来的。
所以它不可能跟 `内核/system/python/bindings.cpp` 走散 —— 那一面变了，重跑一次就对上了。

> 机器只在 Python 这一面露出这么多。**内核、GPU、渲染器、场景文件都不在这儿** —— 那不是说
> 它们不重要，是说这一层绑定有意做窄：前端拿到的是 `System`，不是它底下的机器。要那些，
> 走 C++（见 [`参考/引擎.md`](引擎.md)、[`参考/内核-system.md`](内核-system.md)）。

模块自己的话：*The 烛龙 machine: a kernel, a GPU, an engine and the loop that drives them.*

两个读法上的注：`arg0` 是绑定时**没有起名字**的参数（不是「第一个参数」的意思），
`int` / `float` 是 pybind11 那句很长的 `SupportsInt | SupportsIndex` 折过来的。

---

## 数据 —— 一个场景由这些组成

### `Vec3`

| 属性 | |
|---|---|
| `x` | 可读写 |
| `y` | 可读写 |
| `z` | 可读写 |

---

### `Camera`

| 属性 | |
|---|---|
| `eye` | 可读写 |
| `far_z` | 可读写 |
| `fov_y_radians` | 可读写 |
| `near_z` | 可读写 |
| `target` | 可读写 |
| `up` | 可读写 |

---

### `Light`

| 属性 | |
|---|---|
| `colour` | 可读写 |
| `direction` | 可读写 |

---

### `Texture`

| 属性 | |
|---|---|
| `height` | 只读 |
| `width` | 只读 |

---

### `Material`

| 属性 | |
|---|---|
| `normal_texture` | 可读写 |
| `specular` | 可读写 |
| `texture` | 可读写 |
| `tint` | 可读写 |

---

### `Mesh`

```python
index_count(self: Mesh) -> int
vertex_count(self: Mesh) -> int
```

---

### `Node`

```python
drawable(self: Node) -> bool
```

---

### `Scene`

| 属性 | |
|---|---|
| `ambient` | 可读写 |
| `background` | 可读写 |
| `camera` | 可读写 |
| `shadow_map_size` | 可读写 |

```python
add_light(self: Scene, arg0: Light) -> int
add_material(self: Scene, arg0: Material) -> int
add_mesh(self: Scene, arg0: Mesh) -> int
add_node(self: Scene, mesh: int, material: int, local: Mat4, parent: int = -1) -> int
add_texture(self: Scene, arg0: Texture) -> int
light_count(self: Scene) -> int
material_count(self: Scene) -> int
material_tint(self: Scene, arg0: int) -> list[float]
mesh_count(self: Scene) -> int
node_count(self: Scene) -> int
node_drawable(self: Scene, arg0: int) -> bool
node_material(self: Scene, arg0: int) -> int
node_mesh(self: Scene, arg0: int) -> int
node_parent(self: Scene, arg0: int) -> int
set_material_tint(self: Scene, index: int, tint: Sequence[float]) -> None
set_node_local(self: Scene, index: int, local: Mat4) -> None
set_node_material(self: Scene, index: int, material: int) -> None
texture_count(self: Scene) -> int
```

---

## 常量 —— 不指向任何东西时的下标

```python
kNoMesh = 4294967295
kNoTexture = 4294967295
kNoParent = -1
```

（「空」的那个下标。和 C++ 那边同名的常量是一回事。）

---

## 程序化图元 —— 还没有内容管道，这就是全部的建模词汇

```python
make_box() -> Mesh
make_grid(divisions: int = 1, uv_tiles: float = 1.0) -> Mesh
make_cylinder(sides: int = 16) -> Mesh
make_cone(sides: int = 16) -> Mesh
make_disc(sides: int = 32) -> Mesh
make_sphere(rings: int = 8, segments: int = 16) -> Mesh
make_checker_texture(size: int, cells: int) -> Texture
make_ripple_normal_texture(size: int, cells: int) -> Texture
```

---

## 变换 —— `Mat4` 对 Python 是不透明的，只能用这些造出来

### `Mat4`

```python
__mul__(self: Mat4, arg0: Mat4) -> Mat4
```

---

```python
identity() -> Mat4
translation(x: float, y: float, z: float) -> Mat4
rotation_y(radians: float) -> Mat4
scale(factor: float) -> Mat4
scale_xyz(x: float, y: float, z: float) -> Mat4
radians(degrees: float) -> float
```

---

## 机器 —— 那台机器，和它回调出来的一侧

### `Platform`

```python
on_frame(self: Platform, scene: Scene, seconds: float) -> None
on_mix(self: Platform, scene: Scene, seconds: float) -> None
present(self: Platform, rgba: int, pitch: int, width: int, height: int) -> None
pump(self: Platform) -> bool
```

---

### `FrameResult`

| 成员 | 值 |
|---|---|
| `Rendered` | `0` |
| `Closed` | `1` |
| `Failed` | `2` |

---

### `System`

```python
backend_name(self: System) -> str
draws(self: System) -> int
error(self: System) -> str
faults(self: System) -> int
frame(self: System) -> FrameResult
frames(self: System) -> int
load(self: System, scene: Scene) -> bool
note(self: System) -> str
ok(self: System) -> bool
run(self: System, frames: int) -> int
scene(self: System) -> Scene
set_platform(self: System, platform: Platform) -> None
syncpoint_signals(self: System) -> int
```

---

### `System.Config`

| 属性 | |
|---|---|
| `arena_bytes` | 可读写 |
| `audio_block_frames` | 可读写 |
| `dram_bytes` | 可读写 |
| `frame_interval_ms` | 可读写 |
| `height` | 可读写 |
| `require_vulkan` | 可读写 |
| `vulkan` | 可读写 |
| `width` | 可读写 |

---
