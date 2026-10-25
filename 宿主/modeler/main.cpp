// 烛龙 (ZhuLong) - 建模器：一个宿主程序，画一份模型，让面板能指着它。
//
// 它是最普通的一种宿主：实现 `Platform`，交出去，剩下的由机器驱动（`System::Run` —— 循环不是
// 宿主的，`Run` 里那句注释就是为这件事写的）。它不组装机器，也不自己管 arena。
//
// 今天它会做这些：
//
//   * 把**一串对象**（每个 = 一份 `MeshDescription` + 它摆在哪儿）摆进场景，旁边放一张地面网格
//     （三条坐标轴按 `Y` 才出来，默认不显示）；
//   * 从面板收鼠标：右键拖着转、中键拖着平移、滚轮缩、左键选一个东西然后拖着它走，**按着 Shift
//     拖则吸附到 0.05 的格子上**；
//   * 选中的东西亮着：面是一层亮色的表面，点是一个小方块；
//   * `1` / `2` / `3` 在选面 / 选点 / **选对象**之间切：面/点模式编辑**活动对象**的网格，对象模式
//     点中一整块、按住左键整块拖（改的是它的位置）。`E` 挤出选中的面，`D` 细分活动对象，
//     `B` 把选中的角切掉，
//     `C` 把选中那个面的一圈边标成硬边（细分时它们不跟着变圆），`X` 删掉选中的东西（**对象模式删
//     整个对象**，面模式删那个面、留一个洞），
//     `Y` 开关三条坐标轴，`R` 转选中的对象（绕 Y：按了之后左右移鼠标，左键定下来），`Z` 撤销，
//     `S` 把**活动对象**存回原处、`O` 从原处再读一份（新对象）；
//     「上色」给**选中的东西**换色（对象模式涂整块，面模式涂那个面）。
//   * 编辑器那一排「图元」按钮点一下，它就把那个形状**拿在手里** —— 在画面上左键点一下就放在那里（`P`
//     那一行），放下来是**新加的一个对象**；「打开…」「导入…」也是新加对象。`Esc` / 右键取消。
//
// **它画的是"按材质拆开"的模型。** 引擎那边一个可画节点配一种材质，所以一个模型上有几种颜色就是
// 几个节点 —— 拆在 `Rebuild` 里，管线和着色器一个字不用改。**拾取仍然打整个模型烘出来的那一份**，
// 所以点到的一定是画出来的那一片，而拾取逻辑不用管模型被分成了几块。
//
// **它现在能改拓扑，也能存读了**：挤出多出四个点和四个面，细分把每个面变成四个，而模型存成一份
// 文本文件（`引擎/include/zlong/engine/model_file.h` 里是格式）。存到哪儿不是它挑的 ——
// 编辑器把路径从**管子**里送下来（`S <路径>` / `L <路径>`），因为 Windows 上命令行里的中文到不了
// 这儿（见 `editor_host.h` 那段）。
//
// 它还不能：选边、展开 uv；也没有多选 —— 一次只能选一个面 / 一个点 / 一个对象，没有框选、没有加选。
// 对象**能平移、能绕 X/Y 转**（拖动中按 X/Y/Z 还能锁到一根轴上精确挪）；缩放还没做。硬边是"整个
// 面的一圈"一次标的（`C`），没有"点中某一条边"这回事；贴图落在 uv 本来就在的地方，没有拖 uv 的地方。
//
// 视口就是引擎自己的渲染器，跑在 Vulkan 上（`require_vulkan`，宁可不启动也不悄悄回落到软件
// 光栅器）。所以你对着建模的那块画布，就是游戏将来显示用的那一个，不是它的预览。
//
// 三处顺序值得留意，它们不是随手写的：
//
//   * **相机改了不重新 Load。** 相机是每帧的数据，渲染器每帧从 `scene.camera` 现算矩阵再写
//     per-drawable 常量，所以改它不用重新上传。
//   * **几何或选择变了才重新 Load。** 整份场景重新上传 0.07 毫秒（量过的），比琢磨"哪一块
//     变了"简单得多，也保证画出来的和手上那份描述永远一致。
//   * **拖动从按下那一刻的位置算起，不在上一次的结果上累加**（见 `MoveSelection`）。

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../editor_host.h"
#include "zlong/engine/boolean.h"
#include "zlong/engine/image_file.h"
#include "zlong/engine/mesh.h"
#include "zlong/engine/model_file.h"
#include "zlong/engine/obj_file.h"
#include "zlong/engine/scene_file.h"
#include "zlong/system/system.h"

namespace {

using zlong::engine::Crease;
using zlong::engine::Dot;
using zlong::engine::Cross;
using zlong::engine::Face;
using zlong::engine::Mat4;
using zlong::engine::Mesh;
using zlong::engine::MeshDescription;
using zlong::engine::Normalize;
using zlong::engine::Scene;
using zlong::engine::Texture;
using zlong::engine::Vec3;

// 面板发下来的键号，和 `editor_host.h` 里那套一致。
constexpr int kLeftButton = 1;
constexpr int kRightButton = 2;
constexpr int kMiddleButton = 4;

// 相机离目标多近多远就不让再走了。
constexpr float kNearest = 0.6f;
constexpr float kFarthest = 60.0f;
// 俯仰停在地平线上下一点，别让它翻过顶 —— 翻过去以后 up 和视线共线，画面会扭。
constexpr float kPitchLimit = 1.45f;

// 挑顶点的宽容度，按**画面像素**算。太小学生就得瞄准，太大相邻的点会互相抢。
constexpr float kPickRadius = 12.0f;

// 撤销栈最多留多少步。每步是整串对象一份快照 —— 现在是个位数 KB，但对象会长，所以有个上限。
constexpr std::size_t kUndoDepth = 128;

// 回给编辑器的**对象表**那些行带这个前缀（走 stdout，和文字同一条管子）。打头是一个控制字符 `\x01`，
// 正常输出撞不上它；编辑器那头（`编码器/runtime.py` 的 `SCENE_PREFIX`）按它分流到大纲视图。
constexpr const char* kSceneMark = "\x01" "zl ";

// 每次挤出多少。**定长，不追鼠标** —— 挤完新面是选中的，想放哪儿接着拖就是了（拖动那一套是
// 现成的）。"挤一步再拖"比"挤的时候一路追着指针"好写，也好预料：连按三次就是稳稳的三层。
constexpr float kExtrudeStep = 0.25f;

// 地面网格在哪个高度。**它是一块不透明的格网**，东西掉到它下面就被挡得看不见了 —— 所以摆对象时
// 最低点会被限制在这个高度上（见 `MoveSelection`）。
constexpr float kGroundY = -0.5f;

// 切角切进去多深，**相对边长的一段**。
//
// 不按世界单位算：模型有大有小，一段固定的长度在大东西上什么也切不掉、在小东西上会把它削没。
// 0.28 是看着顺眼的：立方体切出来那八个三角面大小正合适，又不至于把面切成细条。0.5 会让一条边
// 两头的切点撞在一起，所以只能比它小。
constexpr float kBevelFraction = 0.28f;

// 虚拟键码：Shift 是 0x10。
constexpr int kShiftKey = 0x10;

// Shift 拖动的吸附格子，0.05。
//
// **吸的是"拖到哪儿"，不是"拖了多远"。** 取整位移的话，一个本来就偏在格与格之间的东西会永远
// 偏着 —— 而"把它摆到格上"正是按 Shift 的理由。所以取整的是**选择的中点落点**，然后反推位移：
// 形状原样保留，中点落在格上。
//
// 0.05 是挑的：单位大的图元正好是它的整数倍，挤出那 0.25 也是。
constexpr float kSnapStep = 0.05f;

float Snap(float value) { return std::round(value / kSnapStep) * kSnapStep; }

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Mul(const Vec3& v, float scale) { return {v.x * scale, v.y * scale, v.z * scale}; }

/// 绕 +Y 转一个向量 —— 和 `zlong::engine::RotationY` 是同一个方向、同一个约定。
Vec3 SpinY(const Vec3& v, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
}

/// 绕 +X 转一个向量 —— 和 `zlong::engine::RotationX` 一个约定。
Vec3 SpinX(const Vec3& v, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return {v.x, c * v.y + s * v.z, -s * v.y + c * v.z};
}

/// 局部 -> 世界：**先绕 X 再绕 Y**（和 `Object::Local()` 的 `Ry · Rx` 一个顺序）。
Vec3 ObjectSpin(const Vec3& v, float yaw, float pitch) { return SpinY(SpinX(v, pitch), yaw); }

/// 世界 -> 局部：上面那个的逆（`(Ry·Rx)⁻¹ = Rx(−p)·Ry(−y)`）。
Vec3 ObjectSpinInverse(const Vec3& v, float yaw, float pitch) {
    return SpinX(SpinY(v, -yaw), -pitch);
}

/// 对象**局部 -> 世界**：先转（`Ry·Rx`），再按对象的缩放放大，再挪到它的位置。
Vec3 ObjectLocalToWorld(const Vec3& local, const Vec3& position, float yaw, float pitch,
                        float scale) {
    return Add(position, Mul(ObjectSpin(local, yaw, pitch), scale));
}

/// 世界 -> 对象局部：上面那个的逆。
Vec3 ObjectWorldToLocal(const Vec3& world, const Vec3& position, float yaw, float pitch,
                        float scale) {
    const float safe = scale == 0.0f ? 1.0f : scale;
    return Mul(ObjectSpinInverse(Sub(world, position), yaw, pitch), 1.0f / safe);
}

/// 这个网格（按给定转角转过来之后）在 Y 上的范围：最低、最高那个点相对对象原点的偏移。
struct VerticalExtent {
    float low = 0.0f;
    float high = 0.0f;
};

VerticalExtent VerticalRange(const MeshDescription& mesh, float yaw, float pitch) {
    VerticalExtent range;
    if (mesh.positions.empty()) {
        return range;
    }
    range.low = ObjectSpin(mesh.positions[0], yaw, pitch).y;
    range.high = range.low;
    for (const Vec3& position : mesh.positions) {
        const float y = ObjectSpin(position, yaw, pitch).y;
        range.low = std::min(range.low, y);
        range.high = std::max(range.high, y);
    }
    return range;
}

/// 这个网格（按给定转角转过来之后）最低的那个点，相对对象原点的偏移。用来算"它最低能摆到哪"。
float LowestOffset(const MeshDescription& mesh, float yaw, float pitch) {
    return VerticalRange(mesh, yaw, pitch).low;
}

/// 一个对象**在世界里**的轴对齐包围盒（把它的每个点按摆放搬过去）。
struct WorldBounds {
    Vec3 min{};
    Vec3 max{};
    bool valid = false;
};

// 焊接顶点的两个小工具（`zlong::engine::MergeVertices` / `CompressVertices`）住在引擎的 `mesh.h` 里
// —— 布尔运算也要它们，所以只有一份。

/// 路径的最后一段（去掉目录和扩展名），给新对象起名用。不追求严谨 —— 只是个显示名。
std::string BaseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    return name.empty() ? "对象" : name;
}

/// 相机怎么绕着看：一个目标点、一个距离、两个角。
struct Orbit {
    Vec3 target{0.0f, 0.5f, 0.0f};
    float distance = 4.2f;
    float yaw = 0.72f;
    float pitch = 0.42f;

    Vec3 eye() const {
        const float flat = distance * std::cos(pitch);
        return {target.x + flat * std::sin(yaw), target.y + distance * std::sin(pitch),
                target.z + flat * std::cos(yaw)};
    }
};

/// Möller–Trumbore：射线打一个三角形。
///
/// `direction` 必须是单位向量，那样回来的 `distance` 就是世界单位上的距离。平行和打到背面
/// 都算不中 —— 背面那一条是故意的：一片单面几何从背后看不见，能点中它反而怪。
bool Hits(const Vec3& origin, const Vec3& direction, const Vec3& a, const Vec3& b, const Vec3& c,
          float& distance) {
    const Vec3 edge1 = Sub(b, a);
    const Vec3 edge2 = Sub(c, a);
    const Vec3 pvec = Cross(direction, edge2);
    const float determinant = Dot(edge1, pvec);
    if (std::fabs(determinant) < 1e-8f) {
        return false;
    }
    const float inverse = 1.0f / determinant;
    const Vec3 tvec = Sub(origin, a);
    const float u = Dot(tvec, pvec) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3 qvec = Cross(tvec, edge1);
    const float v = Dot(direction, qvec) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float along = Dot(edge2, qvec) * inverse;
    if (!(along > 0.0f)) {
        return false;
    }
    distance = along;
    return true;
}

Vec3 Position(const zlong::engine::MeshVertex& vertex) {
    return {vertex.position[0], vertex.position[1], vertex.position[2]};
}

/// 图元的号。**这是三处同一张表之一** —— 另外两处是 `编码器/main.py` 的 `_MODEL_PRIMITIVES` 和
/// `宿主/editor_host.h` 里 `P` 那一行的说明。改一处就要改三处。
enum class Primitive {
    None = 0,
    Box = 1,
    Grid = 2,
    Disc = 3,
    Cylinder = 4,
    Cone = 5,
    Sphere = 6,
    Torus = 7,
    Pyramid = 8,
    Frustum = 9,
};

/// 建一个图元的描述。号不在表里就是一份空的（调用方看 `faces` 空不空）。
MeshDescription BuildPrimitive(Primitive kind) {
    switch (kind) {
    case Primitive::Box:
        return zlong::engine::DescribeBox();
    case Primitive::Grid:
        return zlong::engine::DescribeGrid(4);
    case Primitive::Disc:
        return zlong::engine::DescribeDisc();
    case Primitive::Cylinder:
        return zlong::engine::DescribeCylinder();
    case Primitive::Cone:
        return zlong::engine::DescribeCone();
    case Primitive::Sphere:
        return zlong::engine::DescribeSphere();
    case Primitive::Torus:
        return zlong::engine::DescribeTorus();
    case Primitive::Pyramid:
        return zlong::engine::DescribePyramid();
    case Primitive::Frustum:
        return zlong::engine::DescribeFrustum();
    default:
        return MeshDescription{};
    }
}

/// 图元的名，用来在控制台说话。
const char* PrimitiveName(Primitive kind) {
    switch (kind) {
    case Primitive::Box:
        return "方块";
    case Primitive::Grid:
        return "网格";
    case Primitive::Disc:
        return "圆盘";
    case Primitive::Cylinder:
        return "圆柱";
    case Primitive::Cone:
        return "圆锥";
    case Primitive::Sphere:
        return "球";
    case Primitive::Torus:
        return "圆环";
    case Primitive::Pyramid:
        return "金字塔";
    case Primitive::Frustum:
        return "圆台";
    default:
        return "?";
    }
}

/// 一个对象：一份**局部空间**的网格，加它摆在场景里的位置。
///
/// 这是 Blender 那个模型的一半 —— 场景是一串对象，每个自带网格、自己摆在哪儿。**对象能平移、能绕
/// X/Y 转**；缩放还没做（旋转在导出时烘进网格，见 `ExportScene`）。
struct Object {
    std::string name;
    MeshDescription mesh;              // 局部空间
    Vec3 position{0.0f, 0.0f, 0.0f};   // 摆在哪
    /// 绕 +Y 的转角（**左右转**）和绕 +X 的俯仰（**上下转**），弧度。
    ///
    /// 旋转只有这两根轴：`.scene` 的摆放指令是 `T · Ry · S`，绕 X 转它表达不了 —— 所以**导出时把旋转烘
    /// 进网格**（见 `ExportScene`），这里就仍是一份可以随时改、随时撤销的变换。绕 Z 的滚转没有（用
    /// 处少，也没手柄）。
    float yaw = 0.0f;
    float pitch = 0.0f;
    /// **均匀**缩放。`.scene` 的摆放指令本来就带 `SCALE`，所以这一份**导出是白送的** —— 不用烘进网格。
    float scale = 1.0f;
    std::string source_dir;            // 贴图路径相对谁（导出时用来找图；空 = 路径本身就能开）

    Mat4 Local() const {
        return zlong::engine::Translation(position.x, position.y, position.z) *
               zlong::engine::RotationY(yaw) * zlong::engine::RotationX(pitch) *
               zlong::engine::Scale(scale);
    }
};

/// 建模器本身。它既是那台机器的宿主，也是这个工具的脑子。
class Modeler final : public zlong::system::Platform {
public:
    Modeler(zlong::system::System& machine, const zlong::system::System::Config& config,
            std::uint32_t shadow_size)
        : machine_(machine), config_(config), shadow_size_(shadow_size) {
        objects_.push_back(Object{"方块", zlong::engine::DescribeBox(), Vec3{0.0f, 0.0f, 0.0f}});
        if (zlong::editor::requested()) {
            editor_ = std::make_unique<zlong::editor::Host>();
        }
        Rebuild();
    }

    /// 机器的宿主这一半：收输入，改场景。返回 false 就是该收工了。
    bool Pump() override {
        if (editor_ == nullptr) {
            return false;              // 没有编辑器就没有输入，也就没有这次运行
        }
        if (!editor_->Pump()) {
            return false;              // 编辑器把这个程序停了
        }
        bool geometry_changed = false;
        for (const zlong::editor::Host::Key& key : editor_->TakeKeys()) {
            // Shift 是吸附的开关。**它自己也是一个键事件**，所以盯着它按没按就够了 —— 拖动的时候
            // 不会有键事件，而"松开了"那一下现在也会送到（见 `editor_host.h` 的 `Key`）。
            if (key.code == kShiftKey) {
                SetSnapping(key.down);
            }
            if (key.down) {
                geometry_changed |= OnKey(key.code);
            }
        }
        // 编辑器交下来的颜色。
        for (const zlong::editor::Host::Colour& colour : editor_->TakeColours()) {
            geometry_changed |= AssignColour({colour.r, colour.g, colour.b, colour.a});
        }

        // 编辑器交下来的存取吩咐。路径从管子来，不从命令行来 —— 见 `editor_host.h` 那段。
        for (const zlong::editor::Host::FileOrder& order : editor_->TakeFiles()) {
            using Kind = zlong::editor::Host::FileOrder::Kind;
            switch (order.kind) {
            case Kind::Save:
                SaveTo(order.path);
                break;
            case Kind::Load:
                geometry_changed |= LoadFrom(order.path);
                break;
            case Kind::Import:
                geometry_changed |= ImportFrom(order.path);
                break;
            case Kind::Texture:
                geometry_changed |= SetTexture(order.path);
                break;
            case Kind::Picture:
                geometry_changed |= InsertImage(order.path);
                break;
            case Kind::Scene:
                ExportScene(order.path);
                break;
            }
        }

        // 编辑器点下来的图元：拿在手里，这一帧（或下一帧）的点击就把它放下。**排在点击前面**，
        // 这样"点图元按钮 + 点画面"落在同一批里也接得上。
        for (const zlong::editor::Host::Placement& placement : editor_->TakePlacements()) {
            if (placement.kind < 1 || placement.kind > 9) {
                continue;              // 号不在表里就当没看见
            }
            armed_ = static_cast<Primitive>(placement.kind);
            std::printf("手里是%s：在画面上左键点一下就放下（Esc / 右键取消）\n", PrimitiveName(armed_));
        }

        // 大纲视图点了一下：换成那个活动对象。**直接切到对象模式** —— 在大纲里选对象是个对象层的
        // 动作，切过去它才会在视口里亮起来。
        for (const int index : editor_->TakeSelections()) {
            if (index < 0 || static_cast<std::size_t>(index) >= objects_.size()) {
                continue;
            }
            active_ = index;
            mode_ = Mode::Object;
            picked_face_ = -1;
            picked_vertex_ = -1;
            SetSelection({index});
            geometry_changed = true;
            std::printf("活动对象：%s\n", objects_[static_cast<std::size_t>(index)].name.c_str());
        }

        for (const zlong::editor::Host::Click& click : editor_->TakeClicks()) {
            // 转 / 缩放 的时候，一次按下就是"定下来"（右键 = 取消）—— 别再当普通的选 / 拖。
            if ((rotating_ || scaling_) && click.down) {
                const Object* object =
                    (picked_object_ >= 0 &&
                     static_cast<std::size_t>(picked_object_) < objects_.size())
                        ? &objects_[static_cast<std::size_t>(picked_object_)]
                        : nullptr;
                if (click.button == kRightButton) {
                    if (rotating_) {
                        CancelRotate();
                    } else {
                        CancelScale();
                    }
                } else if (rotating_) {
                    const float yaw = object != nullptr ? object->yaw : 0.0f;
                    const float pitch = object != nullptr ? object->pitch : 0.0f;
                    rotating_ = false;
                    std::printf("转好了：绕 Y %.1f°、绕 X %.1f°（Z 撤销）\n",
                                static_cast<double>(yaw * 180.0f / zlong::engine::kPi),
                                static_cast<double>(pitch * 180.0f / zlong::engine::kPi));
                } else {
                    const float factor = object != nullptr ? object->scale : 1.0f;
                    scaling_ = false;
                    std::printf("缩放好了：%.2f 倍（Z 撤销）\n", static_cast<double>(factor));
                }
                geometry_changed = true;
                continue;
            }
            if (click.down) {
                held_ |= click.button;
            } else {
                held_ &= ~click.button;
            }
            if (click.down) {
                // 按下的那一刻把上个位置对齐过来，于是这一帧的位移是零 —— 否则一按下去画面
                // 就会跳一下，跳的量正好是"上次移动到现在"的那一段。
                last_pointer_ = editor_->pointer();
            }
            // 右键按下也顺手"取消手里那个图元" —— 它本来就用来转，取消不添乱。
            if (click.button == kRightButton && click.down) {
                armed_ = Primitive::None;
            }
            if (click.button == kLeftButton) {
                if (click.down) {
                    if (armed_ != Primitive::None) {
                        // 手里有图元：这一下是**把它放在这儿**，不是选一个东西。放下之后就不再
                        // 手里拿着了（一次性）—— 想再放一个，回面板上再点一下。
                        geometry_changed |= PlacePrimitive(armed_, click.x, click.y);
                        armed_ = Primitive::None;
                    } else if (mode_ == Mode::Object && snapping_) {
                        // **Shift 点 = 加选 / 减选**（多选就靠它）。这一下不开始拖动。
                        geometry_changed |= Pick(click.x, click.y, /*additive=*/true);
                    } else {
                        geometry_changed |= Pick(click.x, click.y, /*additive=*/false);
                        BeginDrag(click);
                    }
                } else {
                    EndDrag();
                }
            }
        }

        const int notches = editor_->TakeWheel();
        if (notches != 0) {
            const bool have_object = mode_ == Mode::Object && picked_object_ >= 0 &&
                                     static_cast<std::size_t>(picked_object_) < objects_.size();
            if (have_object && !snapping_) {
                // **选了东西：滚轮改它的大小**（乘着走，和缩放手柄一个手感）。一个手势只压一步撤销
                // —— 滚轮每帧来一次，不合并的话转两秒就把 128 步撤销挤光了。
                Object& object = objects_[static_cast<std::size_t>(picked_object_)];
                float factor =
                    object.scale * std::exp(static_cast<float>(notches) * 0.12f);
                factor = std::clamp(factor, 0.05f, 50.0f);
                if (std::fabs(factor - object.scale) > 1e-5f) {
                    const auto now = std::chrono::steady_clock::now();
                    const bool new_gesture =
                        last_wheel_time_.time_since_epoch().count() == 0 ||
                        std::chrono::duration<double>(now - last_wheel_time_).count() > 0.4;
                    last_wheel_time_ = now;
                    if (new_gesture) {
                        PushUndo(Now());
                    }
                    object.scale = factor;
                    geometry_changed = true;
                    std::printf("缩放 %.2f 倍\n", static_cast<double>(factor));
                }
            } else {
                // 没选东西（或者按着 Shift）：滚轮缩**画面**。
                // 乘着走而不是加着走：远离和靠近的手感才是对称的，走多远也不会变慢或变抖。
                orbit_.distance *= std::exp(-static_cast<float>(notches) * 0.12f);
                orbit_.distance = std::clamp(orbit_.distance, kNearest, kFarthest);
            }
        }

        const auto pointer = editor_->pointer();
        const int dx = pointer.x - last_pointer_.x;
        const int dy = pointer.y - last_pointer_.y;
        if (dx != 0 || dy != 0) {
            if ((held_ & kRightButton) != 0) {
                TurnView(dx, dy);
            } else if ((held_ & kMiddleButton) != 0) {
                Pan(dx, dy);
            } else if ((held_ & kLeftButton) != 0) {
                if (dragging_) {
                    geometry_changed |= Drag();
                } else if (snapping_) {
                    Pan(dx, dy);       // Shift + 左键拖空处 = 平移
                } else {
                    // 左键拖**空处**（这一下没选中东西）= **转场景**（上下左右）。中键拖是平移，
                    // 按住 Shift 拖空处也是平移。
                    TurnView(dx, dy);
                }
            }
            last_pointer_ = pointer;
        }

        // 转：左右一动改绕 Y，上下动改绕 X。**不按键也送 `M`**，所以这里就是"按下 R 之后随便移鼠标"。
        if (rotating_ && picked_object_ >= 0 &&
            static_cast<std::size_t>(picked_object_) < objects_.size()) {
            Object& object = objects_[static_cast<std::size_t>(picked_object_)];
            float yaw = rotate_start_yaw_ + static_cast<float>(pointer.x - rotate_from_x_) * 0.006f;
            float pitch =
                rotate_start_pitch_ + static_cast<float>(pointer.y - rotate_from_y_) * 0.006f;
            if (snapping_) {
                const float step = zlong::engine::Radians(15.0f);
                yaw = std::round(yaw / step) * step;
                pitch = std::round(pitch / step) * step;
            }
            if (std::fabs(yaw - object.yaw) > 1e-5f || std::fabs(pitch - object.pitch) > 1e-5f) {
                if (!rotate_undo_) {
                    PushUndo(rotate_before_);   // 第一次真的转了才压栈（和拖动一个规矩）
                    rotate_undo_ = true;
                }
                object.yaw = yaw;
                object.pitch = pitch;
                geometry_changed = true;
            }
        }

        // 缩放：鼠标左右一走，大小**乘着**变（乘着走才是对称的 —— 往右走和往左走是同样"多少倍"）。
        if (scaling_ && picked_object_ >= 0 &&
            static_cast<std::size_t>(picked_object_) < objects_.size()) {
            Object& object = objects_[static_cast<std::size_t>(picked_object_)];
            float factor =
                scale_start_ * std::exp(static_cast<float>(pointer.x - scale_from_x_) * 0.005f);
            if (snapping_) {
                factor = std::round(factor / 0.25f) * 0.25f;
            }
            factor = std::clamp(factor, 0.05f, 50.0f);
            if (std::fabs(factor - object.scale) > 1e-5f) {
                if (!scale_undo_) {
                    PushUndo(scale_before_);
                    scale_undo_ = true;
                }
                object.scale = factor;
                geometry_changed = true;
            }
        }

        Aim();                         // 相机是每帧的数据，改了不用重新上传

        if (geometry_changed) {
            Rebuild();
        }
        return true;
    }

    /// 屏幕上那一帧画好了。`Platform::Present` 的那几个参数，原样交给编辑器。
    void Present(const std::uint8_t* rgba, std::uint32_t pitch, std::uint32_t width,
                 std::uint32_t height) override {
        if (editor_ != nullptr) {
            editor_->Present(rgba, pitch, width, height);
        }
    }

private:
    // **这两个类型要排在前面，而且 `Mode` 要在 `Snapshot` 前面。** 它们会被下面那些方法的签名
    // 用到，而类的成员类型必须在使用它的声明之前就已声明 —— 放进成员那一堆里（最初的写法）会在
    // `PushUndo(const Snapshot&)` 那里报 "unknown type name"，因为那时它还没出现。

    /// 在选什么：一个面、一个点，还是**一整个对象**。
    ///
    /// **对象**是一份网格加它摆在哪儿（`Object`）—— 场景是一串对象，`active_` 是现在编辑的那一个。
    /// 面/点模式动的是**活动对象内部**的点；对象模式动的是整个对象的摆放。这就是 Blender 那两层
    /// （对象层 / 网格层）。
    ///
    /// 用 1 / 2 / 3 切换，不是 Tab：键盘那根管子只送虚拟键码，**不送修饰键**（见 `viewport.py`），
    /// 所以 Ctrl+Z 到这儿和 Z 长得一样。撤销因此就绑在 Z 上。要把 Shift 吸附或者真正的 Ctrl
    /// 组合加回来，得先给那根管子加一位修饰掩码。
    enum class Mode { Face, Vertex, Object };

    /// 一步撤销要放回去的东西：**模型，和当时选着什么**。
    ///
    /// 只放模型是不够的。挤出和细分都会改面的个数，撤销之后那个号可能指到别处、甚至指到外面去
    /// —— 挤了三层再撤三次，模型回到六个面，而选中还停在 17 号上，`Rebuild` 下一个 `[]` 就是
    /// 读别人的内存。选中是编辑状态的一部分，就该跟着一起回退。
    struct Snapshot {
        std::vector<Object> objects;
        int active = 0;
        Mode mode = Mode::Face;
        int face = -1;
        int vertex = -1;
        std::vector<int> selection;   // 选中的对象们（**最后一个点是"主"**）
    };

    /// 现在的这一份，用来压栈。
    Snapshot Now() const {
        return Snapshot{objects_, active_, mode_, picked_face_, picked_vertex_, selection_};
    }

    /// 换一批选中：`selection_` 是全部，`picked_object_` 始终是**最后一个**（"主"对象，面板高亮、
    /// 旋转/缩放、大纲那一行都看它）。两个一起改，省得各处对不齐。
    void SetSelection(std::vector<int> next) {
        selection_ = std::move(next);
        picked_object_ = selection_.empty() ? -1 : selection_.back();
    }

    /// 这个对象在这个批次里选中了没有。
    bool IsSelected(int index) const {
        return std::find(selection_.begin(), selection_.end(), index) != selection_.end();
    }

    /// 这个编号是不是个能用的对象。
    bool ValidObject(int index) const {
        return index >= 0 && static_cast<std::size_t>(index) < objects_.size();
    }

    /// 活动对象。**面/点模式的所有编辑都落在它的网格上** —— 场景里其余对象不动。
    MeshDescription& Active() { return objects_[static_cast<std::size_t>(active_)].mesh; }
    const MeshDescription& Active() const { return objects_[static_cast<std::size_t>(active_)].mesh; }
    const Object& ActiveObject() const { return objects_[static_cast<std::size_t>(active_)]; }

    /// 把每个对象烘一遍（局部空间），并记下"第几个三角形属于第几个面"。拓扑一变就重来。
    ///
    /// 拾取打的是**烘出来的三角形**，不是描述里的多边形 —— 这样点到的一定是画出来的那一片，
    /// 包括四边面是从哪条对角线折开的。
    void BakeObjects() {
        object_mesh_.resize(objects_.size());
        object_tri_face_.resize(objects_.size());
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            std::string error;
            if (!zlong::engine::BakeMesh(objects_[i].mesh, object_mesh_[i], error)) {
                std::printf("对象「%s」烘不出来：%s\n", objects_[i].name.c_str(), error.c_str());
                object_mesh_[i] = Mesh{};
                object_tri_face_[i].clear();
                continue;
            }
            object_tri_face_[i].clear();
            for (std::size_t index = 0; index < objects_[i].mesh.faces.size(); ++index) {
                const int triangles = static_cast<int>(objects_[i].mesh.faces[index].count) - 2;
                for (int one = 0; one < triangles; ++one) {
                    object_tri_face_[i].push_back(static_cast<int>(index));
                }
            }
        }
    }
    /// 把场景重新拼一份：逐对象烘、按材质拆、发节点，再放编辑器外壳（地面、轴、高亮）。
    ///
    /// **不是浪费**：整份重新上传 0.07 毫秒，比琢磨"哪一块变了"简单，也保证画面和手上这份对象表
    /// 永远一致。每个对象的节点带着它自己的摆放（今天是平移）。
    void Rebuild() {
        BakeObjects();

        // 场景重新拼一份。**不是浪费**：整份重新上传 0.07 毫秒，比琢磨"哪一块变了"简单，
        // 也保证画面和手上这份描述永远一致。
        scene_ = Scene();
        scene_.background = {0.09f, 0.10f, 0.13f, 1.0f};
        scene_.ambient = {0.26f, 0.28f, 0.34f};
        scene_.shadow_map_size = shadow_size_;
        scene_.camera.fov_y_radians = zlong::engine::Radians(48.0f);
        scene_.camera.near_z = 0.05f;
        scene_.camera.far_z = 200.0f;

        zlong::engine::Light sun;
        sun.direction = {0.42f, 0.78f, 0.36f};
        sun.colour = {1.0f, 0.97f, 0.90f};
        scene_.lights.push_back(sun);

        zlong::engine::Material ground;
        ground.tint = {0.30f, 0.31f, 0.34f, 1.0f};
        ground.specular = 0.05f;
        scene_.materials.push_back(ground);

        scene_.meshes.push_back(zlong::engine::MakeGrid(24, 12.0f));
        scene_.AddNode(0, 0, zlong::engine::Translation(0.0f, kGroundY, 0.0f) *
                                 zlong::engine::Scale(12.0f, 1.0f, 12.0f));

        // **逐个对象、按材质拆开画。** 引擎那边一个可画节点配一种材质，所以"一个东西上有几种颜色"
        // 在那一侧就是"几个节点" —— 拆在这儿，管线和着色器一个字不用改（见 `SplitByMaterial`）。
        // 每个对象的节点带着它自己的摆放（今天是平移）。
        std::string error;
        std::map<std::string, std::uint32_t> uploaded;   // 同一个图片文件不重复上传
        for (const Object& object : objects_) {
            const Mat4 placed = object.Local();
            for (const zlong::engine::MaterialSplit& split :
                 zlong::engine::SplitByMaterial(object.mesh)) {
                Mesh piece;
                if (!zlong::engine::BakeMesh(split.description, piece, error)) {
                    std::printf("对象「%s」的某一块烘不出来：%s\n", object.name.c_str(),
                                error.c_str());
                    continue;
                }
                zlong::engine::Material worn{split.material.tint, split.material.specular,
                                             zlong::engine::kNoTexture, zlong::engine::kNoTexture};
                // 材质挂着的贴图要搬进场景那一摞里（按文件去重，同一张图不传两遍）。
                if (split.material.texture != zlong::engine::kNoTexture &&
                    split.material.texture < split.description.textures.size()) {
                    const zlong::engine::MeshTexture& picture =
                        split.description.textures[split.material.texture];
                    const auto seen = picture.path.empty() ? uploaded.end()
                                                           : uploaded.find(picture.path);
                    if (seen != uploaded.end()) {
                        worn.texture = seen->second;
                    } else {
                        worn.texture = static_cast<std::uint32_t>(scene_.textures.size());
                        scene_.textures.push_back(picture.image);
                        if (!picture.path.empty()) {
                            uploaded[picture.path] = worn.texture;
                        }
                    }
                }
                const auto mesh = static_cast<std::uint32_t>(scene_.meshes.size());
                scene_.meshes.push_back(std::move(piece));
                const auto material = static_cast<std::uint32_t>(scene_.materials.size());
                scene_.materials.push_back(worn);
                scene_.AddNode(mesh, material, placed);
            }
        }

        // 三条坐标轴是**开关**（默认不显示，按 Y 或者面板上的「坐标轴」才出来）。它是编辑器外壳，
        // 不参与导出。
        if (show_axes_) {
            AddAxes();
        }

        // 选中用的那个亮材质。**它排在最后**，下标是算出来的 —— 模型有几种材质是变的，写死一个
        // 数字的话，模型一上色高亮就会指到别的材质上去。
        zlong::engine::Material hot;
        hot.tint = {1.0f, 0.62f, 0.18f, 1.0f};
        hot.specular = 0.6f;
        hot_material_ = static_cast<std::uint32_t>(scene_.materials.size());
        scene_.materials.push_back(hot);

        AddHighlights();

        if (!machine_.Load(scene_)) {
            std::printf("场景过不去：%s\n", machine_.error().c_str());
        }
        Aim();
        PublishScene();
    }

    /// 把对象表发回编辑器（`kSceneMark` 打头，编辑器按它分流到大纲视图）。
    ///
    /// **只在真的变了的时候发**（对象增删、改名、换活动对象）—— 不然每挪一个面都刷一遍列表，既浪费
    /// 又会和用户正在点的行打架。签名就是"活动号 + 所有名字"。
    void PublishScene() {
        std::string signature = std::to_string(active_);
        for (const Object& object : objects_) {
            signature += "|";
            signature += object.name;
        }
        if (signature == scene_signature_) {
            return;
        }
        scene_signature_ = signature;
        std::printf("%sbegin\n", kSceneMark);
        for (std::size_t index = 0; index < objects_.size(); ++index) {
            std::printf("%sobj %zu %s\n", kSceneMark, index, objects_[index].name.c_str());
        }
        std::printf("%ssel %d\n", kSceneMark, active_);
        std::printf("%send\n", kSceneMark);
    }

    /// 选中的东西亮起来：面高亮那个面、对象模式高亮整块、点放一个小方块。都用那个亮材质。
    void AddHighlights() {
        const MeshDescription& mesh = Active();
        const Object& object = ActiveObject();
        const Mat4 placed = object.Local();
        if (mode_ == Mode::Face && picked_face_ >= 0 &&
            static_cast<std::size_t>(picked_face_) < mesh.faces.size()) {
            MeshDescription one;
            one.positions = mesh.positions;
            one.faces.push_back(mesh.faces[static_cast<std::size_t>(picked_face_)]);
            PushHighlight(one, placed, /*per_vertex_normal=*/false);
        }
        if (mode_ == Mode::Object) {
            // 选中的**每一个**都亮 —— 多选的时候看起来就是"这一批"。
            for (const int index : selection_) {
                if (!ValidObject(index)) {
                    continue;
                }
                const Object& picked = objects_[static_cast<std::size_t>(index)];
                PushHighlight(picked.mesh, picked.Local(), /*per_vertex_normal=*/true);
            }
        }
        if (mode_ == Mode::Vertex && picked_vertex_ >= 0 &&
            static_cast<std::size_t>(picked_vertex_) < mesh.positions.size()) {
            // 一个小方块骑在那个点上。不用点或者十字：那些要么在 IR 里表达不出来（没有 swizzle），
            // 要么就得给渲染器开一条"编辑器专用"的路 —— 而一个缩放过的立方体是现成的。
            const Vec3 at = ObjectLocalToWorld(
                mesh.positions[static_cast<std::size_t>(picked_vertex_)], object.position,
                object.yaw, object.pitch, object.scale);
            scene_.meshes.push_back(zlong::engine::MakeBox());
            const auto marker = static_cast<std::uint32_t>(scene_.meshes.size() - 1);
            scene_.AddNode(marker, hot_material_,
                           zlong::engine::Translation(at.x, at.y, at.z) *
                               zlong::engine::Scale(0.07f));
        }
    }

    /// 把一份描述烘成高亮网格、沿法线推出 0.004，挂在 `placed` 处。
    ///
    /// 推这一下是必须的：引擎没有 polygon offset，两片共面的面会打架，谁赢按片段来，看上去是一条
    /// 抖动的花边。`per_vertex_normal` 为真时每个顶点沿自己的法线推（一整块）；为假时整片沿第一个
    /// 顶点的法线推（一个面是平的，一条法线够了）。
    void PushHighlight(const MeshDescription& description, const Mat4& placed,
                       bool per_vertex_normal) {
        Mesh highlight;
        std::string reason;
        if (!zlong::engine::BakeMesh(description, highlight, reason) || highlight.vertices.empty()) {
            return;
        }
        const float nx = highlight.vertices[0].normal[0];
        const float ny = highlight.vertices[0].normal[1];
        const float nz = highlight.vertices[0].normal[2];
        for (auto& vertex : highlight.vertices) {
            vertex.position[0] += (per_vertex_normal ? vertex.normal[0] : nx) * 0.004f;
            vertex.position[1] += (per_vertex_normal ? vertex.normal[1] : ny) * 0.004f;
            vertex.position[2] += (per_vertex_normal ? vertex.normal[2] : nz) * 0.004f;
        }
        scene_.meshes.push_back(std::move(highlight));
        const auto node = static_cast<std::uint32_t>(scene_.meshes.size() - 1);
        scene_.AddNode(node, hot_material_, placed);
    }

    /// 三条轴。单位立方体拉成细条，四个红绿蓝的材质 —— 在三维里没有它们会分不清朝向。
    void AddAxes() {
        const float length = 2.0f;
        const float thin = 0.012f;
        const std::array<zlong::engine::Material, 3> colours = {{
            {{0.85f, 0.26f, 0.28f, 1.0f}, 0.1f, zlong::engine::kNoTexture,
             zlong::engine::kNoTexture},
            {{0.36f, 0.78f, 0.36f, 1.0f}, 0.1f, zlong::engine::kNoTexture,
             zlong::engine::kNoTexture},
            {{0.30f, 0.52f, 0.92f, 1.0f}, 0.1f, zlong::engine::kNoTexture,
             zlong::engine::kNoTexture},
        }};
        const std::array<Mat4, 3> where = {{
            zlong::engine::Translation(length * 0.5f, -0.5f, 0.0f) *
                zlong::engine::Scale(length, thin, thin),
            zlong::engine::Translation(0.0f, -0.5f + length * 0.5f, 0.0f) *
                zlong::engine::Scale(thin, length, thin),
            zlong::engine::Translation(0.0f, -0.5f, length * 0.5f) *
                zlong::engine::Scale(thin, thin, length),
        }};

        scene_.meshes.push_back(zlong::engine::MakeBox());
        const auto box = static_cast<std::uint32_t>(scene_.meshes.size() - 1);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            scene_.materials.push_back(colours[axis]);
            const auto material = static_cast<std::uint32_t>(scene_.materials.size() - 1);
            scene_.AddNode(box, material, where[axis]);
        }
    }

    /// 相机那三个方向。射线、投影和拖动平面都从它们出来，所以只写一次。
    void CameraBasis(Vec3& forward, Vec3& right, Vec3& up) const {
        forward = Normalize(Sub(orbit_.target, orbit_.eye()));
        right = Normalize(Cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
        up = Cross(right, forward);
    }

    /// 画面上的一个像素，变成一条射线。
    ///
    /// 没有用矩阵求逆：视线的基就是相机那三个方向，加上视场角就能把像素摊到近平面上。少一个
    /// 逆矩阵，也少一处"转置乘反了"的机会。视场角直接读相机的，不另抄一个常量。
    void RayAt(int x, int y, Vec3& origin, Vec3& direction) const {
        const float width = static_cast<float>(config_.width);
        const float height = static_cast<float>(config_.height);

        origin = orbit_.eye();
        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);

        const float half = std::tan(0.5f * scene_.camera.fov_y_radians);
        const float aspect = width / height;
        const float ndc_x = (static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f;
        const float ndc_y = 1.0f - (static_cast<float>(y) + 0.5f) / height * 2.0f;

        direction = Normalize(Add(forward, Add(Mul(right, ndc_x * half * aspect),
                                               Mul(up, ndc_y * half))));
    }

    /// 指针**现在**在哪，那条射线。拖动中每帧都要算它，所以省一个参数。
    ///
    /// 按下的那一下**不走这里** —— 它用事件自己带来的位置，见 `BeginDrag`。
    void AimRay(Vec3& origin, Vec3& direction) const {
        const auto pointer = editor_ != nullptr ? editor_->pointer() : zlong::editor::Host::Pointer{};
        RayAt(pointer.x, pointer.y, origin, direction);
    }

    /// 一个世界点落在哪个画面像素上 —— `AimRay` 的反面。返回 false 表示它在相机后面。
    ///
    /// 挑顶点的判据是**屏幕上隔多远**，不是世界里的距离：离得远的东西那个点看起来就是小的，
    /// 用一个世界单位去量，近处什么也点不中、远处一碰一大片。
    bool ToPixel(const Vec3& point, float& x, float& y) const {
        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        const Vec3 from_eye = Sub(point, orbit_.eye());
        const float along = Dot(from_eye, forward);
        if (!(along > 1e-4f)) {
            return false;
        }
        const float half = std::tan(0.5f * scene_.camera.fov_y_radians);
        const float width = static_cast<float>(config_.width);
        const float height = static_cast<float>(config_.height);
        const float ndc_x = (Dot(from_eye, right) / along) / (half * (width / height));
        const float ndc_y = (Dot(from_eye, up) / along) / half;
        x = (ndc_x + 1.0f) * 0.5f * width;
        y = (1.0f - ndc_y) * 0.5f * height;
        return true;
    }

    /// 左键点一下：看模式 —— 选一个面 / 一个点 / **一个对象**。用的是**这一下自己的位置**，不是
    /// 指针现在在哪（两者可能差一帧的鼠标移动）。返回"要不要重新拼场景"。
    bool Pick(int x, int y, bool additive) {
        switch (mode_) {
        case Mode::Vertex:
            return PickVertex(x, y);
        case Mode::Object:
            return PickObject(x, y, additive);
        default:
            return PickFace(x, y);
        }
    }

    /// 射线打**活动对象**烘出来的三角形，最近的命中属于哪个面。
    ///
    /// 对象只有平移，所以把射线的起点减掉对象位置就进了局部空间（世界距离也就等于局部距离）。
    bool PickFace(int x, int y) {
        Vec3 origin;
        Vec3 direction;
        RayAt(x, y, origin, direction);
        // 进活动对象的局部空间：先减掉它的位置，再把它自己的转角反向转掉。
        const Object& object = ActiveObject();
        origin = ObjectWorldToLocal(origin, object.position, object.yaw, object.pitch, object.scale);
        direction = ObjectSpinInverse(direction, object.yaw, object.pitch);

        const Mesh& mesh = object_mesh_[static_cast<std::size_t>(active_)];
        int best_face = -1;
        float best_distance = 0.0f;
        const std::size_t triangles = mesh.indices.size() / 3;
        for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
            const auto at = static_cast<std::size_t>(triangle) * 3;
            const Vec3 a = Position(mesh.vertices[mesh.indices[at + 0]]);
            const Vec3 b = Position(mesh.vertices[mesh.indices[at + 1]]);
            const Vec3 c = Position(mesh.vertices[mesh.indices[at + 2]]);
            float distance = 0.0f;
            if (Hits(origin, direction, a, b, c, distance) &&
                (best_face < 0 || distance * object.scale < best_distance)) {
                best_face = object_tri_face_[static_cast<std::size_t>(active_)][triangle];
                best_distance = distance * object.scale;
            }
        }

        if (best_face == picked_face_) {
            return false;              // 还是那个面，什么都不用重来
        }
        picked_face_ = best_face;
        if (picked_face_ < 0) {
            std::printf("点空了\n");
        } else {
            std::printf("面 %d / 共 %zu 个，距离 %.2f\n", picked_face_, Active().faces.size(),
                        best_distance);
        }
        return true;
    }

    /// 对象模式：射线打**所有对象**，最近的命中属于哪个就选哪个。换对象时 `active_` 跟着走，
    /// 于是切回面/点模式接着编辑的就是它。
    bool PickObject(int x, int y, bool additive) {
        Vec3 origin;
        Vec3 direction;
        RayAt(x, y, origin, direction);

        int best_object = -1;
        float best_distance = 0.0f;
        for (std::size_t index = 0; index < objects_.size(); ++index) {
            const Object& object = objects_[index];
            const Vec3 local_origin =
                ObjectWorldToLocal(origin, object.position, object.yaw, object.pitch, object.scale);
            const Vec3 local_dir = ObjectSpinInverse(direction, object.yaw, object.pitch);
            const Mesh& mesh = object_mesh_[index];
            const std::size_t triangles = mesh.indices.size() / 3;
            for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
                const auto at = static_cast<std::size_t>(triangle) * 3;
                const Vec3 a = Position(mesh.vertices[mesh.indices[at + 0]]);
                const Vec3 b = Position(mesh.vertices[mesh.indices[at + 1]]);
                const Vec3 c = Position(mesh.vertices[mesh.indices[at + 2]]);
                float distance = 0.0f;
                if (Hits(local_origin, local_dir, a, b, c, distance) &&
                    (best_object < 0 || distance * object.scale < best_distance)) {
                    best_object = static_cast<int>(index);
                    best_distance = distance * object.scale;
                }
            }
        }

        if (best_object < 0) {
            if (!additive && !selection_.empty()) {
                SetSelection({});
                std::printf("点空了（选中清掉）\n");
                return true;
            }
            return false;              // Shift 点空处：什么都不动
        }

        if (additive) {
            // **Shift 点 = 加选 / 减选** —— 多选就靠它。
            const auto found = std::find(selection_.begin(), selection_.end(), best_object);
            const std::string& name = objects_[static_cast<std::size_t>(best_object)].name;
            if (found != selection_.end()) {
                selection_.erase(found);
                picked_object_ = selection_.empty() ? -1 : selection_.back();
                if (picked_object_ >= 0) {
                    active_ = picked_object_;
                }
                std::printf("移出「%s」，还选着 %zu 个\n", name.c_str(), selection_.size());
            } else {
                selection_.push_back(best_object);
                picked_object_ = best_object;
                active_ = best_object;
                std::printf("加上「%s」，现在选着 %zu 个\n", name.c_str(), selection_.size());
            }
            return true;
        }

        if (selection_.size() == 1 && picked_object_ == best_object) {
            return false;              // 还是那一个，什么都不用重来
        }
        SetSelection({best_object});
        active_ = best_object;
        std::printf("选中对象「%s」（%zu 个面），按住左键拖动；**Shift 点可以加选**\n",
                    objects_[static_cast<std::size_t>(best_object)].name.c_str(),
                    objects_[static_cast<std::size_t>(best_object)].mesh.faces.size());
        return true;
        return true;
    }

    /// 挑顶点：屏幕上离指针最近、而且在 `kPickRadius` 之内的那个。
    ///
    /// 并列时取**离相机最近**的。一个凸的东西，背后那几个点会投影到前面那个点附近，这一条就让
    /// 背面点不到 —— 不做遮挡测试也够了。凹的东西不成立，那时候要拿射线打一遍网格去比距离。
    bool PickVertex(int x, int y) {
        const auto want_x = static_cast<float>(x);
        const auto want_y = static_cast<float>(y);
        const float reach = kPickRadius * kPickRadius;

        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        const Vec3 eye = orbit_.eye();

        const Object& object = ActiveObject();
        int best = -1;
        float best_depth = 0.0f;
        for (std::size_t index = 0; index < Active().positions.size(); ++index) {
            const Vec3 world = ObjectLocalToWorld(Active().positions[index], object.position,
                                                  object.yaw, object.pitch, object.scale);
            float x = 0.0f;
            float y = 0.0f;
            if (!ToPixel(world, x, y)) {
                continue;
            }
            const float dx = x - want_x;
            const float dy = y - want_y;
            if (dx * dx + dy * dy > reach) {
                continue;
            }
            const float depth = Dot(Sub(world, eye), forward);
            if (best < 0 || depth < best_depth) {
                best = static_cast<int>(index);
                best_depth = depth;
            }
        }

        if (best == picked_vertex_) {
            return false;
        }
        picked_vertex_ = best;
        if (picked_vertex_ < 0) {
            std::printf("点空了\n");
        } else {
            std::printf("顶点 %d / 共 %zu 个\n", picked_vertex_, Active().positions.size());
        }
        return true;
    }

    /// 键盘。**键码里没有修饰键**（见 `Mode` 那段），所以是几个光秃秃的字母数字。
    ///
    /// 这里**不去挡"拖动当中换模式"**：一次拖动在按下那一刻就把"我要挪哪几个位置、它们原来在哪"
    /// 抓在手里（见 `BeginDrag`），所以模式之后怎么变都影响不到它。曾经用一个 `dragging_` 标志去
    /// 挡，结果挡不住 —— 同一批读进来的键会比鼠标先被处理，于是"这一帧里先按键后按鼠标"和
    /// "先按鼠标后按键"行为不一样。**让那件事不可能发生，比守着一个顺序假设好。**
    bool OnKey(int key) {
        // **拖动中**按 X / Y / Z = 把这一拖锁到那根世界轴上（再按同一根解开来）。平时这三下键是
        // 删除 / 坐标轴 / 撤销，只有"拖还没结束"的时候才当轴锁 —— 和 Blender 一样。
        if (dragging_ && (key == 'X' || key == 'Y' || key == 'Z')) {
            const int want = key == 'X' ? 0 : (key == 'Y' ? 1 : 2);
            drag_axis_ = drag_axis_ == want ? -1 : want;
            if (drag_axis_ < 0) {
                std::printf("拖动方向：自由（跟着画面那个平面）\n");
            } else {
                std::printf("拖动方向：只沿 %c\n", key);
            }
            return false;
        }
        switch (key) {
        case 0x1B:                     // Esc：先取消"转 / 缩放"，再取消"手里拿着的图元"
            if (rotating_) {
                CancelRotate();
                return true;
            }
            if (scaling_) {
                CancelScale();
                return true;
            }
            if (armed_ == Primitive::None) {
                return false;
            }
            armed_ = Primitive::None;
            std::printf("不放了\n");
            return false;
        case 'V': {                    // 复制当前对象（新的一份错开一点，直接就是选中的）
            if (objects_.empty()) {
                return false;
            }
            PushUndo(Now());
            Object copy = objects_[static_cast<std::size_t>(active_)];
            copy.name = UniqueName(copy.name);
            Vec3 forward;
            Vec3 right;
            Vec3 up;
            CameraBasis(forward, right, up);
            copy.position = Add(copy.position, right);   // 沿画面往右错开一个单位，别和原件叠着
            const std::string name = copy.name;
            objects_.push_back(std::move(copy));
            SelectNewest();
            std::printf("复制成「%s」（现在 %zu 个对象，Z 撤销）\n", name.c_str(), objects_.size());
            return true;
        }
        case 'T': {                    // 缩放：按下之后**左右移鼠标**改大小（乘着走）
            if (scaling_) {
                return false;
            }
            if (mode_ != Mode::Object || picked_object_ < 0 ||
                static_cast<std::size_t>(picked_object_) >= objects_.size()) {
                std::printf("先选一个对象（按 3，或者在左边那一列点一下）再缩放\n");
                return false;
            }
            armed_ = Primitive::None;
            scaling_ = true;
            scale_undo_ = false;
            scale_start_ = objects_[static_cast<std::size_t>(picked_object_)].scale;
            scale_from_x_ = editor_ != nullptr ? editor_->pointer().x : 0;
            scale_before_ = Now();
            std::printf("缩放：左右移鼠标（Shift 吸附 0.25），左键定下来，Esc / 右键取消\n");
            return false;
        }
        case 'R': {                    // 转：按下之后**移动鼠标**就转（左右绕 Y、上下绕 X）
            if (rotating_) {
                return false;          // 已经在转了
            }
            if (mode_ != Mode::Object || picked_object_ < 0 ||
                static_cast<std::size_t>(picked_object_) >= objects_.size()) {
                std::printf("先选一个对象（按 3，或者在左边那一列点一下）再转\n");
                return false;
            }
            armed_ = Primitive::None;
            rotating_ = true;
            rotate_undo_ = false;
            const Object& object = objects_[static_cast<std::size_t>(picked_object_)];
            rotate_start_yaw_ = object.yaw;
            rotate_start_pitch_ = object.pitch;
            rotate_from_x_ = editor_ != nullptr ? editor_->pointer().x : 0;
            rotate_from_y_ = editor_ != nullptr ? editor_->pointer().y : 0;
            rotate_before_ = Now();
            std::printf("转：左右移鼠标 = 绕 Y，上下移 = 绕 X（Shift 吸附 15°），"
                        "左键定下来，Esc / 右键取消\n");
            return false;
        }
        case '1':
            if (mode_ == Mode::Face) {
                return false;
            }
            mode_ = Mode::Face;
            picked_vertex_ = -1;
            std::printf("选面（1 面 / 2 点 / 3 对象，Z 撤销）\n");
            return true;
        case '2':
            if (mode_ == Mode::Vertex) {
                return false;
            }
            mode_ = Mode::Vertex;
            picked_face_ = -1;
            std::printf("选点（1 面 / 2 点 / 3 对象，Z 撤销）\n");
            return true;
        case '3':
            if (mode_ == Mode::Object) {
                return false;
            }
            mode_ = Mode::Object;
            picked_face_ = -1;
            picked_vertex_ = -1;
            SetSelection({});
            std::printf("选对象（1 面 / 2 点 / 3 对象，Z 撤销）：点一个图形，按住左键整块拖\n");
            return true;
        case 'Z':
            return Undo();
        case 'E':
            return ExtrudeSelection();
        case 'D':
            return SubdivideModel();
        case 'B':
            return CornerCutSelection();
        case 'C':
            return ToggleCreases();
        case 'X':
            return DeleteSelection();
        case 'J':
            return JoinSelection();
        case 'F':                      // 落：贴到地面 / 叠到正下方那个东西的顶上
            return DropSelection();
        case 'W':                      // 焊：把正好贴在一起的两块合成一张连续的面
            return WeldSelection();
        case 'U':                      // 布尔：并
            return BooleanSelection(zlong::engine::BooleanOp::Union);
        case 'I':                      // 布尔：交
            return BooleanSelection(zlong::engine::BooleanOp::Intersect);
        case 'K':                      // 布尔：差（主对象减去另一个）
            return BooleanSelection(zlong::engine::BooleanOp::Difference);
        case 'Y':                      // 三条坐标轴：开 / 关（面板上的「坐标轴」也发这个键）
            show_axes_ = !show_axes_;
            std::printf(show_axes_ ? "坐标轴：开\n" : "坐标轴：关\n");
            return true;               // 让它重拼一次场景
        case 'S':
            if (model_path_.empty()) {
                std::printf("还不知道存到哪儿 —— 在建模页上按「保存」挑一个地方\n");
                return false;
            }
            SaveTo(model_path_);
            return false;
        case 'O':
            if (model_path_.empty()) {
                std::printf("还不知道从哪儿读 —— 在建模页上按「打开…」挑一个\n");
                return false;
            }
            return LoadFrom(model_path_);
        default:
            return false;
        }
    }

    /// 从一个别人的网格文件（OBJ）导进来，换掉手上的模型。
    ///
    /// **它也压一步撤销** —— 和读自己的模型一样，"挑错了文件"不该丢掉半天活。
    ///
    /// **导入一定是有损的**（六个角的面要扇成三角形、没被任何面用到的点要丢掉），所以它把账报
    /// 出来。改过别人的文件就得说清楚改了什么，否则用户只会觉得"怎么和我导出来的不一样"。
    bool ImportFrom(const std::string& path) {
        MeshDescription imported;
        zlong::engine::ImportReport report;
        std::string reason;
        if (!zlong::engine::LoadObj(path, imported, report, reason)) {
            std::printf("导不进来：%s\n", reason.c_str());
            return false;
        }

        PushUndo(Now());
        // **导进来是一个新对象**，不是顶掉手上的 —— 场景是一串对象，导入就是多加一个。名字取文件的
        // 主名（图元、图片也走同一条"新对象"的路）。
        const float imported_floor = kGroundY - LowestOffset(imported, 0.0f, 0.0f);
        objects_.push_back(Object{BaseName(path), std::move(imported),
                                  Vec3{0.0f, std::max(0.0f, imported_floor), 0.0f}});
        SelectNewest();
        // 别人的文件不记成"我这份要存到哪儿" —— 想留下就「保存」另存一个。
        model_path_.clear();

        std::printf("导入了 %s（新对象，现在 %zu 个）\n", path.c_str(), objects_.size());
        std::printf("  点 %zu → %zu", report.points_in_file, report.points_kept);
        if (report.points_dropped > 0) {
            std::printf("（%zu 个没被面用到，丢了）", report.points_dropped);
        }
        std::printf("\n  面 %zu → %zu", report.faces_in_file, Active().faces.size());
        if (report.faces_fanned > 0) {
            std::printf("（%zu 个五个角以上的扇成了三角形）", report.faces_fanned);
        }
        std::printf("，平 %zu 滑 %zu\n", report.faces_flat, report.faces_smooth);
        if (report.lines_ignored > 0) {
            std::printf("  跳过 %zu 行用不上的（物体名、材质那些）\n", report.lines_ignored);
        }
        return true;
    }

    /// 把**活动对象**存到 `path`，并且记住它 —— 之后按 `S` 就是存回同一个地方。`.model` 格式没变，
    /// 装的就是一个网格。
    void SaveTo(const std::string& path) {
        std::string reason;
        if (!zlong::engine::SaveModel(path, Active(), reason)) {
            std::printf("存不下去：%s\n", reason.c_str());
            return;
        }
        model_path_ = path;
        std::printf("存到 %s（活动对象「%s」：面 %zu，位置 %zu）\n", path.c_str(),
                    ActiveObject().name.c_str(), Active().faces.size(), Active().positions.size());
    }

    /// 从 `path` 读一份进来，**作为新对象**，并记住它。选中一起清掉 —— 那些号是照旧模型来的。
    ///
    /// 读之前压一步撤销：多出一个对象，而"挑错了文件"是不该丢掉半天活的。
    bool LoadFrom(const std::string& path) {
        MeshDescription loaded;
        std::string reason;
        if (!zlong::engine::LoadModel(path, loaded, reason)) {
            std::printf("读不进来：%s\n", reason.c_str());
            return false;
        }
        PushUndo(Now());
        const float loaded_floor = kGroundY - LowestOffset(loaded, 0.0f, 0.0f);
        objects_.push_back(Object{BaseName(path), std::move(loaded),
                                  Vec3{0.0f, std::max(0.0f, loaded_floor), 0.0f}});
        SelectNewest();
        model_path_ = path;
        std::printf("读了 %s → 新对象「%s」：面 %zu（现在 %zu 个）\n", path.c_str(),
                    ActiveObject().name.c_str(), Active().faces.size(), objects_.size());
        return true;
    }

    /// 取消正在进行的旋转：把角度放回去，那一笔撤销也撤掉 —— 这一步当没发生过。
    void CancelRotate() {
        if (!rotating_) {
            return;
        }
        if (picked_object_ >= 0 && static_cast<std::size_t>(picked_object_) < objects_.size()) {
            Object& object = objects_[static_cast<std::size_t>(picked_object_)];
            object.yaw = rotate_start_yaw_;
            object.pitch = rotate_start_pitch_;
        }
        if (rotate_undo_ && !undo_.empty()) {
            undo_.pop_back();
        }
        rotate_undo_ = false;
        rotating_ = false;
        std::printf("旋转取消\n");
    }

    /// 取消正在进行的缩放：把大小放回去，那一笔撤销也撤掉。
    void CancelScale() {
        if (!scaling_) {
            return;
        }
        if (picked_object_ >= 0 && static_cast<std::size_t>(picked_object_) < objects_.size()) {
            objects_[static_cast<std::size_t>(picked_object_)].scale = scale_start_;
        }
        if (scale_undo_ && !undo_.empty()) {
            undo_.pop_back();
        }
        scale_undo_ = false;
        scaling_ = false;
        std::printf("缩放取消\n");
    }

    /// Shift 按下 / 抬起。**只在变了的时候出声** —— 每帧报一遍会把控制台淹掉。
    void SetSnapping(bool on) {
        if (on == snapping_) {
            return;
        }
        snapping_ = on;
        std::printf(on ? "吸附：开（每次挪 0.05）\n" : "吸附：关\n");
    }

    /// 给选中的那个面**所在的那种材质**挂上一张图。
    ///
    /// 挂的是材质，不是那一个面 —— 材质是好几个面共用的（上色那一条也是这个道理）。所以"给这个面
    /// 贴图"常常会同时给另外几个面也贴上，而那正是"它们是一种材质"的含义。
    ///
    /// 图的路径**尽量存成相对这份模型的**：图就在模型旁边的时候，那份模型连图一起挪走还能用；
    /// 不旁边就存绝对路径，能用，但换台机器就找不着了 —— 那件事会说出来的。
    bool SetTexture(const std::string& picture) {
        if (mode_ != Mode::Face || picked_face_ < 0) {
            std::printf("先选一个面再挂贴图（按 1 切到选面，左键点一个面）\n");
            return false;
        }
        Texture image;
        std::string reason;
        if (!zlong::engine::LoadBmp(picture, image, reason)) {
            std::printf("这张图读不了：%s\n", reason.c_str());
            return false;
        }
        const std::string stored = RelativeToModel(picture);
        std::uint32_t slot = zlong::engine::kNoTexture;
        for (std::size_t index = 0; index < Active().textures.size(); ++index) {
            if (Active().textures[index].path == stored) {
                slot = static_cast<std::uint32_t>(index);
                break;
            }
        }
        PushUndo(Now());
        if (slot == zlong::engine::kNoTexture) {
            slot = static_cast<std::uint32_t>(Active().textures.size());
            Active().textures.push_back(zlong::engine::MeshTexture{stored, std::move(image)});
        }
        const std::uint32_t material = Active().faces[static_cast<std::size_t>(picked_face_)].material;
        if (material < Active().materials.size()) {
            Active().materials[material].texture = slot;
        }
        std::printf("材质 %u 挂上 %s（%ux%u）\n", material, stored.c_str(), image.width,
                    image.height);
        if (stored == picture && !model_path_.empty()) {
            std::printf("  它不在模型旁边，所以存的是绝对路径 —— 换台机器就找不着了\n");
        }
        return true;
    }

    /// 一张图的路径，尽量变成**相对这份模型**的。
    std::string RelativeToModel(const std::string& picture) const {
        if (model_path_.empty()) {
            return picture;            // 还不知道模型存到哪儿，没有"旁边"可言
        }
        const std::size_t slash = model_path_.find_last_of("/\\");
        if (slash == std::string::npos) {
            return picture;
        }
        const std::string folder = model_path_.substr(0, slash + 1);
        if (picture.size() > folder.size() && picture.compare(0, folder.size(), folder) == 0) {
            return picture.substr(folder.size());
        }
        // 大小写不一样就当作不在旁边 —— 那样只是存了个绝对路径，仍然能用。
        return picture;
    }

    /// 找一种一样的材质槽（**看 tint 一样就算一样**），没有就加一个，返回它的下标。
    ///
    /// "同一个颜色复用同一个槽"这条规矩就住在这里 —— 上色用它，放图元也用它（图元自带的那种材质
    /// 也得落到全局槽上，不然 `SplitByMaterial` 会把越界的面按 0 号槽算，颜色就丢了）。
    /// 在**指定的那一份**网格里找一种一样的材质槽（看 tint），没有就加一个。
    ///
    /// 分成"指定的那一份"是因为合并两个对象时要往"底"那一份里加 —— 那个不一定是 `Active()`。
    std::uint32_t AddMaterialTo(MeshDescription& mesh, const zlong::engine::MeshMaterial& material) {
        for (std::size_t index = 0; index < mesh.materials.size(); ++index) {
            const auto& was = mesh.materials[index].tint;
            if (was[0] == material.tint[0] && was[1] == material.tint[1] &&
                was[2] == material.tint[2] && was[3] == material.tint[3]) {
                return static_cast<std::uint32_t>(index);
            }
        }
        mesh.materials.push_back(material);
        return static_cast<std::uint32_t>(mesh.materials.size() - 1);
    }

    /// 在活动对象里找一种一样的材质槽，没有就加一个。
    std::uint32_t FindOrAddMaterial(const zlong::engine::MeshMaterial& material) {
        return AddMaterialTo(Active(), material);
    }

    /// 给**选中的东西**换一个颜色：**对象模式下涂整个对象，面模式下涂那一个面**。
    ///
    /// **同一个颜色复用同一个材质槽**（见 `FindOrAddMaterial`）。每次新建一个的话，涂十次就有
    /// 十种材质，拆出来的网格也跟着长 —— 而"两个面是同一种红"在这份数据里本来就该是一件事。
    bool AssignColour(const std::array<float, 4>& tint) {
        const bool whole = mode_ == Mode::Object;
        std::vector<std::uint32_t> faces;
        if (mode_ == Mode::Face && picked_face_ >= 0) {
            faces.push_back(static_cast<std::uint32_t>(picked_face_));
        } else if (whole && picked_object_ >= 0 &&
                   static_cast<std::size_t>(picked_object_) < objects_.size()) {
            for (std::size_t index = 0; index < Active().faces.size(); ++index) {
                faces.push_back(static_cast<std::uint32_t>(index));
            }
        } else {
            std::printf("先选一个对象（按 3，或者在左边那一列点一下）或者一个面（按 1）再上色\n");
            return false;
        }
        if (faces.empty()) {
            return false;
        }

        zlong::engine::MeshMaterial wanted;
        wanted.tint = tint;
        wanted.specular = Active().materials.empty() ? 0.3f : Active().materials.front().specular;
        const std::uint32_t slot = FindOrAddMaterial(wanted);

        bool any_changed = false;
        for (const std::uint32_t face : faces) {
            if (Active().faces[face].material != slot) {
                any_changed = true;
                break;
            }
        }
        if (!any_changed) {
            return false;              // 已经是那个颜色了，什么都不用重来
        }
        PushUndo(Now());
        for (const std::uint32_t face : faces) {
            Active().faces[face].material = slot;
        }
        std::printf("%s，材质 %u（%.2f, %.2f, %.2f）：%zu 个面（现在 %zu 种）\n",
                    whole ? "整块换色" : "换一个面的颜色", slot, tint[0], tint[1], tint[2],
                    faces.size(), Active().materials.size());
        return true;
    }

    /// 记下这一步之前的样子。**上限到了就丢掉最老的那一步** —— 快照是一整份模型，而挤出和
    /// 细分都会让它长大。
    void PushUndo(const Snapshot& before) {
        undo_.push_back(before);
        if (undo_.size() > kUndoDepth) {
            undo_.erase(undo_.begin());
        }
    }

    /// 新建一个对象之后把它选中 —— 和 Blender 一样，"加一个东西"之后它就在手上：切到对象模式、
    /// 设成活动对象。于是紧接着按「上色」「导出」或者拖它，都直接作用在刚放下的那个上。
    void SelectNewest() {
        active_ = static_cast<int>(objects_.size()) - 1;
        mode_ = Mode::Object;
        SetSelection({active_});
        picked_face_ = -1;
        picked_vertex_ = -1;
    }

    /// 起一个不重名的对象名：`base` 用过了就 `base2`、`base3`……（只是个显示名）。
    std::string UniqueName(const std::string& base) const {
        std::string candidate = base;
        int suffix = 2;
        for (;;) {
            bool taken = false;
            for (const Object& object : objects_) {
                if (object.name == candidate) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                return candidate;
            }
            candidate = base + std::to_string(suffix++);
        }
    }

    /// 把 `kind` 这一个图元放在画面上 `(x, y)` 指的地方 —— 作为场景里**新加的一个对象**。
    ///
    /// 落点是**过相机目标点、正对相机的那个平面**（拖动用的就是它）：射线和它永远有交点；对着地面
    /// 算，视线一压低就算不出来。"屏幕点在那儿、深度在目标那个深度上"也就是这个平面。
    ///
    /// **这是一个新对象**（不再是往当前网格里塞一块壳）—— 场景是一串对象，图元、导入、图片都走
    /// 这条"新对象"的路。所以放下的东西能整块选中、整块拖，导出时也是一个独立的 `.model`。
    bool PlacePrimitive(Primitive kind, int x, int y) {
        MeshDescription shell = BuildPrimitive(kind);
        if (shell.faces.empty()) {
            return false;
        }

        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        Vec3 origin;
        Vec3 direction;
        RayAt(x, y, origin, direction);
        Vec3 at;
        if (!PlaneHit(orbit_.target, origin, direction, forward, at)) {
            std::printf("放不下：射线和那个平面平行\n");
            return false;
        }
        // 一放就放到地面以下是不合理的（新对象没有转角，最低点就是它局部的最低点）。
        at.y = std::max(at.y, kGroundY - LowestOffset(shell, 0.0f, 0.0f));

        PushUndo(Now());   // 改之前压一步；上面没命中就已经返回，不留空步

        const std::string name = UniqueName(PrimitiveName(kind));
        objects_.push_back(Object{name, std::move(shell), at});
        SelectNewest();        // 新放的这一个就是选中的，紧接着上色/拖动都作用在它上
        std::printf("放了「%s」在 (%.2f, %.2f, %.2f)（现在 %zu 个对象，Z 撤销）\n", name.c_str(),
                    at.x, at.y, at.z, objects_.size());
        return true;
    }

    /// 往场景里放一张图：一块**正对相机**的四边形，贴着这张图（Blender 的"图片当平面"）。
    ///
    /// 插入时朝向就烘进它的点里了（朝着当时相机看的方向）—— 想换朝向，选中它按 `R` 转。长宽比按图来，
    /// 不拉伸。
    bool InsertImage(const std::string& picture) {
        Texture image;
        std::string reason;
        if (!zlong::engine::LoadBmp(picture, image, reason)) {
            std::printf("这张图读不了：%s\n", reason.c_str());
            return false;
        }
        const std::uint32_t image_w = image.width;
        const std::uint32_t image_h = image.height;

        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        Vec3 origin;
        Vec3 direction;
        RayAt(static_cast<int>(config_.width) / 2, static_cast<int>(config_.height) / 2, origin,
              direction);
        Vec3 at;
        if (!PlaneHit(orbit_.target, origin, direction, forward, at)) {
            std::printf("放不下：射线和那个平面平行\n");
            return false;
        }

        const float aspect =
            image_h == 0 ? 1.0f : static_cast<float>(image_w) / static_cast<float>(image_h);
        const Vec3 rv = Mul(right, 0.5f * aspect);
        const Vec3 up_half = Mul(up, 0.5f);

        MeshDescription plane;
        plane.materials.push_back(
            zlong::engine::MeshMaterial{{1.0f, 1.0f, 1.0f, 1.0f}, 0.10f, 0u});
        plane.textures.push_back(zlong::engine::MeshTexture{picture, std::move(image)});
        plane.positions = {
            Sub(Sub(Vec3{}, rv), up_half),   // 左下
            Sub(Add(Vec3{}, rv), up_half),   // 右下
            Add(Add(Vec3{}, rv), up_half),   // 右上
            Add(Sub(Vec3{}, rv), up_half),   // 左上
        };
        Face face;
        face.count = 4;
        face.smooth = false;
        face.material = 0;
        for (std::uint32_t corner = 0; corner < 4; ++corner) {
            face.corners[corner] = corner;
            face.uv[corner][0] = (corner == 1 || corner == 2) ? 1.0f : 0.0f;
            face.uv[corner][1] = (corner >= 2) ? 1.0f : 0.0f;
        }
        plane.faces.push_back(face);
        at.y = std::max(at.y, kGroundY - LowestOffset(plane, 0.0f, 0.0f));

        PushUndo(Now());
        const std::string name = UniqueName("图片");
        objects_.push_back(Object{name, std::move(plane), at});
        SelectNewest();
        std::printf("插入图片「%s」在 (%.2f, %.2f, %.2f)（%ux%u，现在 %zu 个对象）\n", name.c_str(),
                    at.x, at.y, at.z, image_w, image_h, objects_.size());
        return true;
    }

    /// 把整个场景导出成一个 `.scene`：每个对象落到同级一个 `.model`，贴图拷到同级，再写场景文件。
    ///
    /// 游戏读的就是这个（`LoadScene` 那一套）。**只含 `objects_`** —— 地面网格、三条轴、高亮那些是
    /// 编辑器外壳，不在里面。
    void ExportScene(const std::string& path) {
        zlong::engine::SceneDescription world;
        world.camera = scene_.camera;
        world.lights = scene_.lights;
        world.ambient = scene_.ambient;
        world.background = scene_.background;
        world.shadow_map_size = shadow_size_;
        world.objects.reserve(objects_.size());
        for (const Object& object : objects_) {
            // **旋转烘进网格里。** `.scene` 的摆放指令只写得了绕 Y 转，绕 X 转它表达不了 —— 所以
            // 这里先把对象的角度转进**它的一份网格副本**（模型器手上那份不动），写出去的 `model`
            // 就只带平移。于是"导出的场景"和"你在画面里看到的"精确一致，而格式一个字没改。
            MeshDescription baked = object.mesh;
            if (std::fabs(object.yaw) > 1e-6f || std::fabs(object.pitch) > 1e-6f) {
                for (Vec3& position : baked.positions) {
                    position = ObjectSpin(position, object.yaw, object.pitch);
                }
            }
            world.objects.push_back(zlong::engine::SceneObjectInput{baked, object.source_dir,
                                                                   object.position, object.scale});
        }
        std::string reason;
        if (!zlong::engine::SaveScene(path, world, reason)) {
            std::printf("场景导不出来：%s\n", reason.c_str());
            return;
        }
        std::printf("导出场景 %s（%zu 个对象 + 相机 + %zu 盏灯）\n", path.c_str(), objects_.size(),
                    world.lights.size());
    }

    /// 挤出选中的那个面。
    ///
    /// 新面**在最后一个**（引擎那边的约定，见 `ExtrudeFace`），所以挤完就选中它：连按几次就是
    /// 往上长几层，而"挤完再拖到想放的地方"用的就是现成的那套拖动。
    bool ExtrudeSelection() {
        if (mode_ != Mode::Face || picked_face_ < 0) {
            std::printf("先选一个面（按 1 切到选面）\n");
            return false;
        }
        MeshDescription edited;
        std::string reason;
        if (!zlong::engine::ExtrudeFace(Active(), static_cast<std::uint32_t>(picked_face_),
                                        kExtrudeStep, edited, reason)) {
            std::printf("挤不出来：%s\n", reason.c_str());
            return false;
        }
        PushUndo(Now());
        Active() = std::move(edited);
        picked_face_ = static_cast<int>(Active().faces.size()) - 1;
        std::printf("挤出 %.2f：面 %zu，位置 %zu，选中 %d\n", kExtrudeStep, Active().faces.size(),
                    Active().positions.size(), picked_face_);
        return true;
    }

    /// Catmull-Clark 细分一次，**整个模型**。
    ///
    /// 面的下标全变了，所以旧的选中没有意义 —— 清掉，别让它指着一个巧合。用户想接着干活就再点
    /// 一下。
    bool SubdivideModel() {
        MeshDescription edited;
        std::string reason;
        if (!zlong::engine::Subdivide(Active(), edited, reason)) {
            std::printf("细分不了：%s\n", reason.c_str());
            return false;
        }
        const std::size_t faces_before = Active().faces.size();
        const std::size_t points_before = Active().positions.size();
        PushUndo(Now());
        Active() = std::move(edited);
        mode_ = Mode::Face;
        picked_face_ = -1;
        picked_vertex_ = -1;
        std::printf("细分：面 %zu → %zu，位置 %zu → %zu（选中已清）\n", faces_before,
                    Active().faces.size(), points_before, Active().positions.size());
        return true;
    }

    /// 切掉选中的角 —— 点模式切那一个点，面模式切那个面的四个角。
    ///
    /// 面的下标会全变，所以切完选中清掉（和细分一个道理）：留着只会指着一个巧合。
    bool CornerCutSelection() {
        std::vector<std::uint32_t> corners;
        if (mode_ == Mode::Vertex) {
            if (picked_vertex_ < 0) {
                std::printf("先点一个顶点（按 2 切到选点）\n");
                return false;
            }
            corners.push_back(static_cast<std::uint32_t>(picked_vertex_));
        } else {
            if (picked_face_ < 0) {
                std::printf("先选一个面（按 1 切到选面）\n");
                return false;
            }
            const Face& face = Active().faces[static_cast<std::size_t>(picked_face_)];
            for (std::uint32_t corner = 0; corner < face.count; ++corner) {
                corners.push_back(face.corners[corner]);
            }
        }

        MeshDescription edited;
        std::string reason;
        if (!zlong::engine::CornerCut(Active(), corners, kBevelFraction, edited, reason)) {
            std::printf("切不了：%s\n", reason.c_str());
            return false;
        }
        const std::size_t faces_before = Active().faces.size();
        const std::size_t points_before = Active().positions.size();
        PushUndo(Now());
        Active() = std::move(edited);
        mode_ = Mode::Face;
        picked_face_ = -1;
        picked_vertex_ = -1;
        std::printf("切角 %.0f%%：面 %zu → %zu，位置 %zu → %zu（选中已清）\n",
                    kBevelFraction * 100.0f, faces_before, Active().faces.size(), points_before,
                    Active().positions.size());
        return true;
    }

    /// 把选中那个面的**四条边**标成硬边 —— 再按一次就取消。
    ///
    /// 为什么要绑在面上而不是边上：这台工具到今天只选面或者点，没有"选边"这回事，而硬边是一张
    /// **边的表**（见 `Crease`）。把"这一个面的框"当一次操作，就够拼出想要的那圈硬边了 —— 想要
    /// 一条硬边连着走过去，就沿着它把两边的面各按一次。
    bool ToggleCreases() {
        if (mode_ != Mode::Face || picked_face_ < 0) {
            std::printf("先选一个面（硬边是「这个面的一圈」，按 1 切到选面）\n");
            return false;
        }
        const Face& face = Active().faces[static_cast<std::size_t>(picked_face_)];
        std::vector<Crease> ring;
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            const std::uint32_t a = face.corners[corner];
            const std::uint32_t b = face.corners[(corner + 1) % face.count];
            if (a != b) {
                ring.push_back(Crease{a < b ? a : b, a < b ? b : a});
            }
        }

        const auto lookup = [this](const Crease& edge) {
            return std::binary_search(Active().creases.begin(), Active().creases.end(), edge);
        };
        const bool all_hard =
            std::all_of(ring.begin(), ring.end(), lookup);

        PushUndo(Now());
        if (all_hard) {
            std::vector<Crease> kept;
            for (const Crease& edge : Active().creases) {
                if (std::find(ring.begin(), ring.end(), edge) == ring.end()) {
                    kept.push_back(edge);
                }
            }
            Active().creases = std::move(kept);
            std::printf("这个面的 %zu 条边不硬了（还剩 %zu 条硬边）\n", ring.size(),
                        Active().creases.size());
        } else {
            for (const Crease& edge : ring) {
                if (!lookup(edge)) {
                    Active().creases.push_back(edge);
                }
            }
            std::sort(Active().creases.begin(), Active().creases.end());
            std::printf("这个面的 %zu 条边硬了（现在 %zu 条硬边）—— 细分的时候它们不会变圆\n",
                        ring.size(), Active().creases.size());
        }
        return true;
    }

    /// 一个对象**在世界里**的轴对齐包围盒（把它的每个点按摆放搬过去）。
    WorldBounds BoundsOf(const Object& object) const {
        WorldBounds bounds;
        for (const Vec3& point : object.mesh.positions) {
            const Vec3 world =
                ObjectLocalToWorld(point, object.position, object.yaw, object.pitch, object.scale);
            if (!bounds.valid) {
                bounds.min = world;
                bounds.max = world;
                bounds.valid = true;
            } else {
                bounds.min.x = std::min(bounds.min.x, world.x);
                bounds.min.y = std::min(bounds.min.y, world.y);
                bounds.min.z = std::min(bounds.min.z, world.z);
                bounds.max.x = std::max(bounds.max.x, world.x);
                bounds.max.y = std::max(bounds.max.y, world.y);
                bounds.max.z = std::max(bounds.max.z, world.z);
            }
        }
        return bounds;
    }

    /// 把选中的对象**落下去**：落到地面上，或者落到它正下方那个对象的顶上（于是能叠起来）。
    ///
    /// 这是搭场景最常用的一下 —— "把这个箱子放到那个箱子上面""把桌子腿落到地上"。一个对象一个对象算：
    /// 它 XZ 的投影和谁交叠，就取那些对象里**顶面最高的一个**当落脚面；没人交叠就是地面。
    bool DropSelection() {
        if (mode_ != Mode::Object || selection_.empty()) {
            std::printf("先选一个对象（按 3，或者在左边那一列点一下）再让它落下去\n");
            return false;
        }
        // 每个对象的包围盒先算一遍（省得每个点重算）。
        std::vector<WorldBounds> bounds(objects_.size());
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            bounds[i] = BoundsOf(objects_[i]);
        }

        PushUndo(Now());
        bool moved = false;
        for (const int index : selection_) {
            if (!ValidObject(index)) {
                continue;
            }
            Object& object = objects_[static_cast<std::size_t>(index)];
            const float low = LowestOffset(object.mesh, object.yaw, object.pitch) * object.scale;
            float support = kGroundY;
            const WorldBounds& self = bounds[static_cast<std::size_t>(index)];
            if (self.valid) {
                for (std::size_t other = 0; other < objects_.size(); ++other) {
                    if (static_cast<int>(other) == index) {
                        continue;
                    }
                    const WorldBounds& box = bounds[other];
                    if (!box.valid) {
                        continue;
                    }
                    // XZ 投影交叠（留一点余量，边挨着边也算）。
                    const bool overlap =
                        self.min.x <= box.max.x + 1e-4f && self.max.x + 1e-4f >= box.min.x &&
                        self.min.z <= box.max.z + 1e-4f && self.max.z + 1e-4f >= box.min.z;
                    if (overlap && box.max.y > support) {
                        support = box.max.y;
                    }
                }
            }
            const float want = support - low;
            if (std::fabs(want - object.position.y) > 1e-5f) {
                object.position.y = want;
                moved = true;
            }
        }
        if (!moved) {
            undo_.pop_back();
            std::printf("已经落着了，没动\n");
            return false;
        }
        std::printf("落下了 %zu 个对象（Z 撤销）\n", selection_.size());
        return true;
    }

    /// 布尔运算：把选中的**两个**对象做并 / 交 / 差。
    ///
    /// 取**主对象**（最后点的那个）当 `A` —— 结果留在它身上（名字、摆放都不动），另一个删掉。运算在
    /// **世界空间**做，做完再换回 `A` 的局部空间，所以两块摆在哪都不影响。至少要选两个。
    bool BooleanSelection(zlong::engine::BooleanOp op) {
        if (mode_ != Mode::Object || selection_.size() < 2) {
            std::printf("布尔运算要**选中两个**对象（对象模式下 Shift 点加选）\n");
            return false;
        }
        std::vector<int> order;
        for (const int index : selection_) {
            if (ValidObject(index) &&
                std::find(order.begin(), order.end(), index) == order.end()) {
                order.push_back(index);
            }
        }
        if (order.size() < 2) {
            std::printf("合法的对象不到两个\n");
            return false;
        }
        const int kept = order.back();     // A = 主对象
        const int gone = order.front();    // B = 另一个
        if (order.size() > 2) {
            std::printf("（选了 %zu 个，只用两个：A = 主对象，B = 另一个）\n", order.size());
        }

        Object& object_a = objects_[static_cast<std::size_t>(kept)];
        Object& object_b = objects_[static_cast<std::size_t>(gone)];
        // 世界空间里算：两块各自把点搬过去。
        MeshDescription world_a = object_a.mesh;
        MeshDescription world_b = object_b.mesh;
        for (Vec3& position : world_a.positions) {
            position = ObjectLocalToWorld(position, object_a.position, object_a.yaw,
                                          object_a.pitch, object_a.scale);
        }
        for (Vec3& position : world_b.positions) {
            position = ObjectLocalToWorld(position, object_b.position, object_b.yaw,
                                          object_b.pitch, object_b.scale);
        }

        MeshDescription result;
        std::string reason;
        if (!zlong::engine::BooleanMesh(world_a, world_b, op, result, reason)) {
            std::printf("算不出来：%s\n", reason.c_str());
            return false;
        }
        // 换回 A 的局部空间。
        for (Vec3& position : result.positions) {
            position = ObjectWorldToLocal(position, object_a.position, object_a.yaw,
                                          object_a.pitch, object_a.scale);
        }

        PushUndo(Now());
        objects_[static_cast<std::size_t>(kept)].mesh = std::move(result);
        objects_.erase(objects_.begin() + gone);
        const int base = kept > gone ? kept - 1 : kept;   // gone < kept，删完要减一
        SetSelection({base});
        active_ = base;
        mode_ = Mode::Object;
        picked_face_ = -1;
        picked_vertex_ = -1;
        const char* name = op == zlong::engine::BooleanOp::Union      ? "并集"
                           : op == zlong::engine::BooleanOp::Intersect ? "交集"
                                                                       : "差集";
        std::printf("%s算完：面 %zu（Z 撤销）\n", name,
                    objects_[static_cast<std::size_t>(base)].mesh.faces.size());
        return true;
    }

    /// 把选中的对象**合并成一个**（Blender 的 Join）。
    ///
    /// 取编号最小的那个当"底"：把别的对象**按各自的摆放搬到世界、再换进底对象的局部空间**收进去
    /// （点、面、材质、贴图、硬边都并），然后把它们删掉。合完只剩一个对象，它是选中的。
    ///
    /// 于是"桌面 + 四条腿"能拼成一张桌子，之后**整块搬、整块转、整块缩**，导出也是**一份 `.model`**。
    bool JoinSelection() {
        if (selection_.size() < 2) {
            std::printf("先选两个以上对象再合并（对象模式下 **Shift 点可以加选**）\n");
            return false;
        }
        std::vector<int> order = selection_;
        std::sort(order.begin(), order.end());
        order.erase(std::unique(order.begin(), order.end()), order.end());
        order.erase(std::remove_if(order.begin(), order.end(),
                                   [this](int index) { return !ValidObject(index); }),
                    order.end());
        if (order.size() < 2) {
            std::printf("合法的对象不到两个，合并不了\n");
            return false;
        }

        PushUndo(Now());
        const int base = order.front();          // 编号最小的当底 —— 删别的不会动它的下标
        Object& ground = objects_[static_cast<std::size_t>(base)];

        for (std::size_t k = 1; k < order.size(); ++k) {
            const Object& other = objects_[static_cast<std::size_t>(order[k])];
            const std::uint32_t offset = static_cast<std::uint32_t>(ground.mesh.positions.size());

            // 点：对方局部 -> 世界 -> 底的局部。
            for (const Vec3& point : other.mesh.positions) {
                const Vec3 world =
                    ObjectLocalToWorld(point, other.position, other.yaw, other.pitch, other.scale);
                ground.mesh.positions.push_back(ObjectWorldToLocal(
                    world, ground.position, ground.yaw, ground.pitch, ground.scale));
            }
            // 贴图：并进底那一摞（按路径去重），记下下标怎么挪。
            std::vector<std::uint32_t> tex_map(other.mesh.textures.size(),
                                               zlong::engine::kNoTexture);
            for (std::size_t t = 0; t < other.mesh.textures.size(); ++t) {
                const zlong::engine::MeshTexture& picture = other.mesh.textures[t];
                std::uint32_t slot = zlong::engine::kNoTexture;
                for (std::size_t u = 0; u < ground.mesh.textures.size(); ++u) {
                    if (ground.mesh.textures[u].path == picture.path) {
                        slot = static_cast<std::uint32_t>(u);
                        break;
                    }
                }
                if (slot == zlong::engine::kNoTexture) {
                    slot = static_cast<std::uint32_t>(ground.mesh.textures.size());
                    ground.mesh.textures.push_back(picture);
                }
                tex_map[t] = slot;
            }
            // 材质：并进底的表，记下映射。
            std::vector<std::uint32_t> mat_map;
            for (const zlong::engine::MeshMaterial& material : other.mesh.materials) {
                zlong::engine::MeshMaterial copy = material;
                if (copy.texture != zlong::engine::kNoTexture && copy.texture < tex_map.size()) {
                    copy.texture = tex_map[copy.texture];
                }
                mat_map.push_back(AddMaterialTo(ground.mesh, copy));
            }
            // 面：角加上位置偏移，材质换成底表里的号。
            for (Face face : other.mesh.faces) {
                for (std::uint32_t corner = 0; corner < face.count; ++corner) {
                    face.corners[corner] += offset;
                }
                face.material = face.material < mat_map.size() ? mat_map[face.material] : 0u;
                ground.mesh.faces.push_back(face);
            }
            // 硬边：位置下标也加偏移，最后再排一次（表要保持有序）。
            for (const Crease& crease : other.mesh.creases) {
                ground.mesh.creases.push_back(Crease{crease.low + offset, crease.high + offset});
            }
        }
        std::sort(ground.mesh.creases.begin(), ground.mesh.creases.end());

        // 把并进去的那些删掉（从后往前，下标不串）。底是最小的号，删完它的下标不变。
        for (std::size_t k = order.size(); k-- > 1;) {
            objects_.erase(objects_.begin() + order[k]);
        }
        SetSelection({base});
        active_ = base;
        mode_ = Mode::Object;
        picked_face_ = -1;
        picked_vertex_ = -1;
        std::printf("合并成「%s」：%zu 个并成一个（面 %zu，Z 撤销）\n",
                    objects_[static_cast<std::size_t>(base)].name.c_str(), order.size(),
                    objects_[static_cast<std::size_t>(base)].mesh.faces.size());
        return true;
    }

    /// 焊接：把**正好贴在一起的两块**合成**一张连续的面**。
    ///
    /// 做法很直白 —— 选中多个先把它们并成一个（和 `J` 同一步），再在那一份网格里：
    ///   1. 把**落在一起的点并成一个**；
    ///   2. 找出**共用同一组角、朝向正好相反**的两个面 —— 那是两块贴在一起时留下的**内部夹层**，
    ///      两片一起删掉（留下的接口由周围的面接上）。
    /// 于是"两个方块面对面贴着"焊完就是**一个干净的盒子**，中间没有夹层。
    ///
    /// **只管"正好贴着"的**（面完全重合）—— 不像布尔那样能切出交线。搭场景时用图元 + `F` 落下 / 吸附
    /// 对齐，面基本就是正好贴上的，这条够用；要斜着插进去、在墙上挖个洞，那是布尔运算，另一个量级。
    bool WeldSelection() {
        if (mode_ != Mode::Object) {
            std::printf("焊接要在对象模式下（按 3）\n");
            return false;
        }
        bool pushed = false;
        if (selection_.size() >= 2) {
            // 先并成一个 —— `JoinSelection` 压的那一步在合并**之前**，所以一次 `Z` 能把"合并 + 焊接"
            // 整个退回去（对用户来说这就是一步）。
            if (!JoinSelection()) {
                return false;
            }
            pushed = true;
        }
        if (!ValidObject(active_)) {
            std::printf("先选一个对象（按 3，或者在左边那一列点一下）再焊\n");
            return false;
        }
        MeshDescription& mesh = Active();
        const std::size_t faces_before = mesh.faces.size();
        const std::size_t points_before = mesh.positions.size();
        if (!pushed) {
            PushUndo(Now());
        }

        // 1. 落在一起的点并成一个。
        const std::vector<std::uint32_t> remap =
            zlong::engine::MergeVertices(mesh.positions, 1e-3f);
        for (Face& face : mesh.faces) {
            for (std::uint32_t corner = 0; corner < face.count; ++corner) {
                face.corners[corner] = remap[face.corners[corner]];
            }
        }
        // 2. 共用同一组角、法线正好相反的一对面 = 内部夹层，两片都删。
        std::map<std::vector<std::uint32_t>, std::vector<std::size_t>> twins;
        for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
            const Face& face = mesh.faces[i];
            std::vector<std::uint32_t> corners(face.corners, face.corners + face.count);
            std::sort(corners.begin(), corners.end());
            corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
            twins[corners].push_back(i);
        }
        std::vector<bool> drop(mesh.faces.size(), false);
        std::size_t hidden = 0;
        for (const auto& entry : twins) {
            if (entry.second.size() != 2) {
                continue;              // 只有"正好两个面用同一组角"才可能是夹层
            }
            float na[3];
            float nb[3];
            const float la =
                zlong::engine::detail::NewellNormal(mesh, mesh.faces[entry.second[0]], na);
            const float lb =
                zlong::engine::detail::NewellNormal(mesh, mesh.faces[entry.second[1]], nb);
            if (la < 1e-12f || lb < 1e-12f) {
                continue;
            }
            const float dot = (na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2]) / (la * lb);
            if (dot < -0.999f) {
                drop[entry.second[0]] = true;
                drop[entry.second[1]] = true;
                hidden += 2;
            }
        }
        std::vector<Face> kept;
        kept.reserve(mesh.faces.size());
        for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
            if (!drop[i]) {
                kept.push_back(mesh.faces[i]);
            }
        }
        mesh.faces = std::move(kept);
        zlong::engine::CompressVertices(mesh);

        if (mesh.faces.size() == faces_before && mesh.positions.size() == points_before) {
            if (!pushed) {
                undo_.pop_back();
            }
            std::printf("没找到正好贴在一起的面（焊接只管正好贴着的），没动\n");
            return false;
        }
        std::printf("焊接：面 %zu → %zu（删了 %zu 片内部夹层），点 %zu → %zu（Z 撤销）\n",
                    faces_before, mesh.faces.size(), hidden, points_before, mesh.positions.size());
        return true;
    }


    /// 删掉选中的东西：**对象模式删选中的每一个对象**，**面模式下删那个面**（在那儿留一个洞）。
    ///
    /// 面那一条是唯一一个让面**变少**的操作，所以没人指着的点会跟着丢掉、剩下的重排 —— 面的下标和点
    /// 的下标都会变，选中照例清掉。点模式按它没有意义（"把这个点周围的面都删了"是另一个操作），明说。
    bool DeleteSelection() {
        if (mode_ == Mode::Object) {
            if (selection_.empty()) {
                std::printf("先选一个对象（按 3 点它，或者在左边那一列点一下）再删\n");
                return false;
            }
            std::vector<int> order = selection_;
            std::sort(order.begin(), order.end(), [](int a, int b) { return a > b; });  // 从后往前删
            PushUndo(Now());
            std::size_t removed = 0;
            for (const int index : order) {
                if (ValidObject(index)) {
                    objects_.erase(objects_.begin() + index);
                    ++removed;
                }
            }
            // **最少留一个对象** —— 手上总得有东西可编辑：拾取、上色、`Rebuild` 都指着 `active_`。
            bool refilled = false;
            if (objects_.empty()) {
                objects_.push_back(
                    Object{"方块", zlong::engine::DescribeBox(), Vec3{0.0f, 0.0f, 0.0f}});
                refilled = true;
            }
            active_ = std::clamp(active_, 0, static_cast<int>(objects_.size()) - 1);
            SetSelection({});
            picked_face_ = -1;
            picked_vertex_ = -1;
            std::printf("删了 %zu 个对象（现在 %zu 个，Z 撤销）%s\n", removed, objects_.size(),
                        refilled ? "；删光了，给你放了一个新方块" : "");
            return true;
        }
        if (mode_ != Mode::Face || picked_face_ < 0) {
            std::printf("先选一个对象（按 3）或者一个面（按 1）再删\n");
            return false;
        }
        MeshDescription edited;
        std::string reason;
        const auto which = static_cast<std::uint32_t>(picked_face_);
        if (!zlong::engine::DeleteFace(Active(), which, edited, reason)) {
            std::printf("删不了：%s\n", reason.c_str());
            return false;
        }
        const std::size_t faces_before = Active().faces.size();
        const std::size_t points_before = Active().positions.size();
        PushUndo(Now());
        Active() = std::move(edited);
        picked_face_ = -1;
        picked_vertex_ = -1;
        std::printf("删掉面 %u：面 %zu → %zu，位置 %zu → %zu（选中已清）\n", which, faces_before,
                    Active().faces.size(), points_before, Active().positions.size());
        return true;
    }

    /// 撤销：把上一份快照放回去 —— **整串对象、活动对象、和选中**一起。
    ///
    /// 不试着"反着做"那一步 —— 逆操作要每一个操作都写对，而快照只需要有一次是对的。
    bool Undo() {
        if (undo_.empty()) {
            std::printf("没有可撤销的了\n");
            return false;
        }
        const Snapshot& step = undo_.back();
        objects_ = step.objects;
        active_ = step.active;
        mode_ = step.mode;
        picked_face_ = step.face;
        picked_vertex_ = step.vertex;
        SetSelection(step.selection);
        undo_.pop_back();
        std::printf("撤销：对象 %zu 个，活动对象 %zu 面，还剩 %zu 步\n", objects_.size(),
                    Active().faces.size(), undo_.size());
        return true;
    }

    /// 选中的那个东西的中心，**世界坐标** —— 拖动平面要穿过它，吸附也是在这个点上取整。
    Vec3 SelectionCentre() const {
        const Object& object = ActiveObject();
        const Vec3 place = object.position;
        if (mode_ == Mode::Object) {
            return (picked_object_ >= 0 &&
                    static_cast<std::size_t>(picked_object_) < objects_.size())
                       ? objects_[static_cast<std::size_t>(picked_object_)].position
                       : place;
        }
        if (mode_ == Mode::Vertex && picked_vertex_ >= 0) {
            return ObjectLocalToWorld(Active().positions[static_cast<std::size_t>(picked_vertex_)],
                                      place, object.yaw, object.pitch, object.scale);
        }
        Vec3 sum{0.0f, 0.0f, 0.0f};
        if (picked_face_ < 0) {
            return place;
        }
        const Face& face = Active().faces[static_cast<std::size_t>(picked_face_)];
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            sum = Add(sum, Active().positions[face.corners[corner]]);
        }
        return ObjectLocalToWorld(Mul(sum, 1.0f / static_cast<float>(face.count)), place,
                                  object.yaw, object.pitch, object.scale);
    }

    bool HasSelection() const {
        return (mode_ == Mode::Vertex && picked_vertex_ >= 0) ||
               (mode_ == Mode::Face && picked_face_ >= 0) ||
               (mode_ == Mode::Object && picked_object_ >= 0 &&
                static_cast<std::size_t>(picked_object_) < objects_.size());
    }

    /// 一条射线打在一个平面上，落在哪。平面由**它上面的一点**和法线给出。
    ///
    /// `plane` 是个参数，不是成员：拖动用的平面过选择，放置图元用的平面过相机目标点 —— 两处都走
    /// 这一条，谁也不用知道对方那个平面存在哪。
    bool PlaneHit(const Vec3& plane, const Vec3& origin, const Vec3& direction, const Vec3& normal,
                  Vec3& hit) const {
        const float denominator = Dot(direction, normal);
        if (std::fabs(denominator) < 1e-6f) {
            return false;
        }
        const float along = Dot(Sub(plane, origin), normal) / denominator;
        if (along <= 0.0f) {
            return false;              // 平面在相机后面
        }
        hit = Add(origin, Mul(direction, along));
        return true;
    }

    /// 左键按下：选一下，然后准备拖。
    ///
    /// 拖动的平面是**过选择、正对相机**的那一个，不是地面也不是某个轴平面。理由是它永远交得上：
    /// 射线本来就朝着这个方向，所以永远不会和平面平行；对着地面拖，视线一压低就再也算不出交点，
    /// 拖动会突然停住或者跳走。
    ///
    /// 代价是**沿视线方向的那一段看不出来** —— 那样拖只能在一个平面里走。轴向约束和吸附是后面
    /// 的事（而且要 Shift，见 `Mode` 那段）。
    ///
    /// 起算点是**这一下按下时的位置**（`click.x` / `click.y`），不是指针现在在哪。按下的那一帧里
    /// 常常还跟着一次移动，去问指针就拿不到手起手落的那一段，拖出来会比手短一截。
    void BeginDrag(const zlong::editor::Host::Click& click) {
        dragging_ = false;
        drag_positions_.clear();
        drag_origins_.clear();
        drag_axis_ = -1;               // 每一拖从"自由"开始
        if (!HasSelection()) {
            return;
        }

        // **挪谁、从哪挪，在这里定下来。** 之后再有人换模式、改选择，都改不了这一拖要对哪些
        // 位置下手；`drag_origins_` 是它们此刻的值，`MoveSelection` 每次从它算起。
        //
        // 对象模式不挪点 —— 它挪**对象的位置**（`MoveSelection` 里另一条路），所以这里留空。
        if (mode_ == Mode::Vertex) {
            drag_positions_.push_back(static_cast<std::uint32_t>(picked_vertex_));
        } else if (mode_ == Mode::Face) {
            const Face& face = Active().faces[static_cast<std::size_t>(picked_face_)];
            for (std::uint32_t corner = 0; corner < face.count; ++corner) {
                drag_positions_.push_back(face.corners[corner]);
            }
        }
        for (const std::uint32_t index : drag_positions_) {
            drag_origins_.push_back(Active().positions[index]);
        }
        // 一整批（`selection_`）的起始位置和"最低点"都抓下来 —— 拖动从这一刻算起。
        drag_object_from_.clear();
        drag_object_low_.clear();
        for (const int index : selection_) {
            if (ValidObject(index)) {
                const Object& object = objects_[static_cast<std::size_t>(index)];
                drag_object_from_.push_back(object.position);
                drag_object_low_.push_back(
                    LowestOffset(object.mesh, object.yaw, object.pitch) * object.scale);
            } else {
                drag_object_from_.push_back(Vec3{});   // 占位，和 `selection_` 对齐
                drag_object_low_.push_back(0.0f);
            }
        }

        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        drag_plane_ = SelectionCentre();

        Vec3 origin;
        Vec3 direction;
        RayAt(click.x, click.y, origin, direction);
        if (!PlaneHit(drag_plane_, origin, direction, forward, drag_from_)) {
            drag_positions_.clear();
            drag_origins_.clear();
            return;
        }
        // 按下那一刻的整份状态也留一份 —— 撤销要放回去的就是它，模型和选中一起。
        drag_before_ = Now();
        pushed_undo_ = false;
        dragging_ = true;
    }

    /// 指针动了，还在按着左键。
    bool Drag() {
        if (!dragging_) {
            return false;
        }
        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);

        Vec3 origin;
        Vec3 direction;
        AimRay(origin, direction);
        Vec3 now;
        if (!PlaneHit(drag_plane_, origin, direction, forward, now)) {
            return false;
        }
        const Vec3 raw = Sub(now, drag_from_);
        Vec3 delta = raw;
        if (snapping_) {
            // `drag_plane_` 就是按下那一刻选择的中点，所以 `drag_plane_ + raw` 是它现在到的地方。
            const Vec3 landed = Add(drag_plane_, raw);
            delta = Sub(Vec3{Snap(landed.x), Snap(landed.y), Snap(landed.z)}, drag_plane_);
        }
        // 锁了轴就只留那一个分量（吸附先做，所以锁完仍然落在格子上）。
        if (drag_axis_ >= 0) {
            const Vec3 axis = drag_axis_ == 0   ? Vec3{1.0f, 0.0f, 0.0f}
                              : drag_axis_ == 1 ? Vec3{0.0f, 1.0f, 0.0f}
                                                : Vec3{0.0f, 0.0f, 1.0f};
            delta = Mul(axis, Dot(delta, axis));
        }
        // 吸附之后如果还是零，就当这一帧没动 —— 于是轻轻一拖不会占掉一步撤销。
        if (std::fabs(delta.x) + std::fabs(delta.y) + std::fabs(delta.z) < 1e-5f) {
            return false;              // 还没动，别让一次纯点击占掉一步撤销
        }
        if (!pushed_undo_) {
            // **第一次真的改了才压栈**，压的是改之前那份。一次没有拖动的点击因此不留痕迹。
            PushUndo(drag_before_);
            pushed_undo_ = true;
        }
        MoveSelection(delta);
        return true;
    }

    void EndDrag() {
        // 只在**真的改过**的时候出声，而且报出那几个位置现在在哪 —— 一次编辑该留下一个能对得上
        // 的结果，不只是"完成了"。一次没拖动的点击也会走完按下抬起，那不该看起来像编辑。
        if (dragging_ && pushed_undo_) {
            const Vec3 centre = MovedCentre();
            std::printf("移动完成，中心 (%.3f, %.3f, %.3f)（Z 撤销）\n", centre.x, centre.y,
                        centre.z);
        }
        dragging_ = false;
        drag_positions_.clear();
        drag_origins_.clear();
    }

    /// 把这一拖抓住的东西挪 `delta`（世界位移）。
    ///
    /// **从按下那一刻的值算起**，不是在当前位置上累加：这一函数每帧都被调用，累加会把浮点误差
    /// 攒起来，而且一放一按就该重新起算。
    ///
    /// 对象模式挪的是**对象的位置**；面/点模式挪的是**位置下标对应的点**（局部空间，而对象只有平移，
    /// 所以世界位移就等于局部位移）。面模式下动的是它那几个**角**，角是**位置的下标** —— 和邻居共用的
    /// 那个点会一起走，这是焊接那件事的另一面，不是 bug。
    void MoveSelection(const Vec3& delta) {
        if (mode_ == Mode::Object) {
            // **一整批一起走** —— 选了几个就搬几个。
            for (std::size_t i = 0; i < selection_.size(); ++i) {
                const int index = selection_[i];
                if (!ValidObject(index)) {
                    continue;
                }
                Object& object = objects_[static_cast<std::size_t>(index)];
                const Vec3 from =
                    i < drag_object_from_.size() ? drag_object_from_[i] : object.position;
                const float low = i < drag_object_low_.size() ? drag_object_low_[i] : 0.0f;
                object.position = Add(from, delta);
                // **别让它掉到地面以下**（地面不透明，掉下去就看不见了）。最低点含缩放。
                const float lowest_allowed = kGroundY - low;
                if (object.position.y < lowest_allowed) {
                    object.position.y = lowest_allowed;
                }
            }
            return;
        }
        // 点是局部空间的，世界位移要**反向转掉转角、再除以缩放**才等于局部位移。
        const Object& active = ActiveObject();
        const float safe = active.scale == 0.0f ? 1.0f : active.scale;
        const Vec3 local_delta =
            Mul(ObjectSpinInverse(delta, active.yaw, active.pitch), 1.0f / safe);
        for (std::size_t index = 0; index < drag_positions_.size(); ++index) {
            Active().positions[drag_positions_[index]] = Add(drag_origins_[index], local_delta);
        }
    }

    /// 这一拖抓住的东西，现在在哪（**世界坐标**）—— 报了它，一次编辑就有了一个能对得上的结果。
    Vec3 MovedCentre() const {
        if (mode_ == Mode::Object) {
            return objects_[static_cast<std::size_t>(active_)].position;
        }
        Vec3 sum{0.0f, 0.0f, 0.0f};
        if (drag_positions_.empty()) {
            return sum;
        }
        for (const std::uint32_t index : drag_positions_) {
            sum = Add(sum, Active().positions[index]);
        }
        const Object& object = ActiveObject();
        return ObjectLocalToWorld(Mul(sum, 1.0f / static_cast<float>(drag_positions_.size())),
                                  object.position, object.yaw, object.pitch, object.scale);
    }

    /// 拖着转**视角**：左右改 yaw、上下改 pitch（俯仰卡在地平线上下一点，翻过顶 up 和视线共线会扭）。
    void TurnView(int dx, int dy) {
        orbit_.yaw -= static_cast<float>(dx) * 0.011f;
        orbit_.pitch = std::clamp(orbit_.pitch + static_cast<float>(dy) * 0.011f, -kPitchLimit,
                                  kPitchLimit);
    }

    /// 中键拖着平移：沿相机自己的右和上走，不沿世界的轴 —— 转过之后手感才是同一套。
    void Pan(int dx, int dy) {
        const Vec3 eye = orbit_.eye();
        const Vec3 forward = Normalize(Sub(orbit_.target, eye));
        const Vec3 right = Normalize(Cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
        const Vec3 up = Cross(right, forward);
        // 屏幕上的一像素，在目标那个平面上大约是多少。和距离成正比，所以拉远了走得快。
        const float pixels = orbit_.distance * 0.0016f;
        orbit_.target = Add(orbit_.target,
                            Add(Mul(right, -static_cast<float>(dx) * pixels),
                                Mul(up, static_cast<float>(dy) * pixels)));
    }

    /// 把相机摆到 orbit 说的位置。它不是上传，所以随时可以改。
    ///
    /// 手上那份 `scene_` 和机器里那份一起改 —— 只改机器那份也行（下次 Rebuild 会重来），但两份
    /// 说法不一致是以后会咬人的那种事。
    void Aim() {
        for (zlong::engine::Scene* scene : {&scene_, &machine_.scene()}) {
            scene->camera.eye = orbit_.eye();
            scene->camera.target = orbit_.target;
            scene->camera.up = {0.0f, 1.0f, 0.0f};
        }
    }

    zlong::system::System& machine_;
    zlong::system::System::Config config_;
    std::uint32_t shadow_size_ = 0;   // 永远由构造参数给；这里跟着默认值，免得两个默认值打架
    std::unique_ptr<zlong::editor::Host> editor_;

    // 场景就是这一串对象。**面/点模式的编辑只作用于 `active_` 那一个**；对象模式作用于整块。
    std::vector<Object> objects_;
    int active_ = 0;

    // 拾取用的缓存，**每个对象一份、局部空间**：烘出来的网格，和第几个三角形属于第几个面。
    // `Rebuild` 里按对象重填。
    std::vector<Mesh> object_mesh_;
    std::vector<std::vector<int>> object_tri_face_;

    Scene scene_;
    /// 选中用的那个亮材质在 `scene_.materials` 里的下标。**算出来的，不是写死的** —— 模型有几
    /// 种材质是会变的。
    std::uint32_t hot_material_ = 0;
    /// 手上这份模型从哪儿来 / 要存到哪儿。空着就是还没说过 —— 那种时候 `S` / `O` 会明说，
    /// 而不是猜一个地方写下去。
    std::string model_path_;
    /// 上一次发回编辑器的对象表签名（见 `PublishScene`）—— 一样就不重发。
    std::string scene_signature_;

    Orbit orbit_;
    int held_ = 0;                     // 哪些键正按着，按位
    bool snapping_ = false;            // Shift 有没有按着
    bool show_axes_ = false;           // 三条坐标轴显不显示（默认不显示，按 Y 切）

    // 正在转（按 R 起、移鼠标转、左键定下来 / Esc 取消）。见 `OnKey` 的 'R' 和 `Pump` 里那一段。
    bool rotating_ = false;
    float rotate_start_yaw_ = 0.0f;    // 按下 R 那一刻的角度（取消要放回去、起算也从它）
    float rotate_start_pitch_ = 0.0f;
    int rotate_from_x_ = 0;            // 按下 R 那一刻的指针（左右算 yaw、上下算 pitch）
    int rotate_from_y_ = 0;
    bool rotate_undo_ = false;         // 第一步真的转了才压栈（和拖动一个规矩）
    Snapshot rotate_before_;

    // 缩放（按 T 起）：和转一个套路 —— 起、移鼠标、左键定 / Esc 取消。
    bool scaling_ = false;
    float scale_start_ = 1.0f;
    int scale_from_x_ = 0;
    bool scale_undo_ = false;
    Snapshot scale_before_;
    /// 上一次滚轮改缩放是什么时候 —— 用来把一次连续滚动**并成一步撤销**。
    std::chrono::steady_clock::time_point last_wheel_time_{};
    zlong::editor::Host::Pointer last_pointer_;

    Mode mode_ = Mode::Face;
    /// 手里拿着的图元。非 None 时，下一次左键是"放在这儿"而不是"选一个"。
    Primitive armed_ = Primitive::None;
    int picked_face_ = -1;             // 活动对象里的面下标
    int picked_vertex_ = -1;           // 活动对象 mesh 的 positions 下标
    int picked_object_ = -1;           // 对象模式选中的对象 —— **`selection_` 里最后一个（"主"）**
    /// 对象模式**选中的全部**对象（可以多个）。`picked_object_` 永远是它的最后一个 —— 两个一起由
    /// `SetSelection` 改。见 `JoinSelection`（合并）、`MoveSelection`（整批搬）。
    std::vector<int> selection_;

    // 正在拖的那一段。**每个数字都有用**：`drag_before_` 是按下那一刻的整份状态（撤销要用），
    // `drag_positions_` 和 `drag_origins_` 是这一拖要挪的位置、以及它们按下时的值。
    //
    // 把"挪谁、从哪挪"**在按下那一刻定下来**，是这一次设计里最要紧的一条：拖动从那里起算，
    // 于是中途换模式、改选择、甚至撤销，都动不了正在进行的这一拖。
    bool dragging_ = false;
    bool pushed_undo_ = false;
    Snapshot drag_before_;
    std::vector<std::uint32_t> drag_positions_;
    std::vector<Vec3> drag_origins_;
    /// 拖动中锁到哪根世界轴：-1 = 自由（跟着相机那个平面），0/1/2 = 锁到 X/Y/Z。
    /// 拖动中按 X / Y / Z 切换（见 `OnKey`）—— 那三下键平时是删除 / 坐标轴 / 撤销，只有"拖还没结束"
    /// 的时候才当轴锁用（和 Blender 一样）。
    int drag_axis_ = -1;
    /// 对象模式：按下那一刻，**选中的每一个**对象的位置、以及它"最低点相对原点"的世界偏移
    /// （含缩放）。两个列表和 `selection_` 一一对应 —— 一整批一起搬，见 `MoveSelection`。
    std::vector<Vec3> drag_object_from_;
    std::vector<float> drag_object_low_;
    Vec3 drag_plane_;
    Vec3 drag_from_;

    /// 撤销栈：**整个编辑状态一份快照**，不是逆操作。
    ///
    /// 一个几千顶点的模型就是几百 KB，复制一下远比写一套"怎么把这一步反回去"简单，也不会因为
    /// 某个操作没写逆而慢慢错开。真建模器写逆操作是因为它们的模型有上百万面，我们不是。
    std::vector<Snapshot> undo_;
};

}  // namespace

int main(int argc, char** argv) {
    // stdout 重定向出去的时候是**全缓冲**的：跑在编辑器里，它那头是一根管子，一个字节不到
    // 4KB 就一直待在缓冲区里 —— 用户点一下面，控制台要等退出才看得见那一行。所以这里不缓冲。
    // （C++ 编出来的宿主都有这件事，不只是这一个。）
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 它没有自己的窗口：画面走面板，鼠标和键盘也走面板。没有编辑器就没有它。
    if (!zlong::editor::requested()) {
        std::printf("建模器要在编辑器里跑 —— 它没有自己的窗口，画面和输入都走那块面板。\n");
        return 1;
    }

    zlong::system::System::Config config;
    // 默认值是从量出来的数里挑的（这一台机器，Vulkan，六个可画节点）：
    //
    //     视口        submit     fps        阴影      submit     fps
    //     800×500      18.8 ms   53.7        关                    53.7
    //     1024×640     25.2 ms   33.6        256²     12.2 ms         58.3   (320×200 量)
    //                                        512²     31.9 ms         31.4
    //                                        1024²   114.9 ms          9.1
    //
    // 于是：**800×500 且不要阴影图**。800×500 是 56fps 档（05-编码器 那张表里的），比
    // 1024×640 的 33 帧明显活。而阴影图那一列是独立的、和视口无关的一笔账 —— 它和面积的
    // 平方成正比，说明那张目标每帧都在整张来回搬。建模的时候影子有用（判断东西落没落在地上），
    // 所以它是 `--shadow 1024` 一句话的事；但把它当默认，工具就只有 9 帧了。
    config.width = 800;
    config.height = 500;
    std::uint32_t shadow_size = 0;
    for (int index = 1; index + 1 < argc; ++index) {
        const std::string name = argv[index];
        const int value = std::atoi(argv[index + 1]);
        if (value > 0 && name == "--width") {
            config.width = static_cast<std::uint32_t>(value);
            ++index;
        } else if (value > 0 && name == "--height") {
            config.height = static_cast<std::uint32_t>(value);
            ++index;
        } else if (value >= 0 && name == "--shadow") {
            // 阴影图是这笔账里最贵的一样，而且是和视口无关的一笔：它是 1024² 上一张 float 目标，
            // 每帧整张来回搬一遍（见上面那张表）。建模时影子帮你判断东西落没落在地上，值得手动
            // 打开；默认关。想要就跑 `--shadow 1024`，编辑器工具栏那个参数框里写就行。
            shadow_size = static_cast<std::uint32_t>(value);
            ++index;
        }
    }
    config.dram_bytes = 512ull * 1024 * 1024;
    config.arena_bytes = 64ull * 1024 * 1024;
    config.vulkan = true;
    // 建模器不接受软件光栅器：它每秒要画几十帧给人看，回落过去不是"慢一点"，是不能用。宁可
    // 起不来说清楚为什么。
    config.require_vulkan = true;
    config.frame_interval_ms = 16.0;

    zlong::system::System machine(config);
    if (!machine.ok()) {
        std::printf("机器起不来：%s\n", machine.error().c_str());
        return 1;
    }
    std::printf("backend: %s\n", machine.backend_name());
    if (!machine.note().empty()) {
        std::printf("note: %s\n", machine.note().c_str());
    }

    Modeler modeler(machine, config, shadow_size);
    // **交出去。** 少了这一行，机器就不知道自己有个宿主：`Frame` 既不调 `Pump`（于是鼠标键盘
    // 没人读），也不调 `Present`（于是画面哪儿都不去）—— 它只是闷头往自己的缓冲里画，一帧一帧
    // 地转下去，看起来像卡死。
    machine.set_platform(&modeler);

    const auto started = std::chrono::steady_clock::now();
    const int drawn = machine.Run(-1);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // 这台工具是拿来盯着看的，所以"每秒多少帧"和"时间花在哪"跟它的功能一样要紧 —— 后者是渲染器
    // 自己的账（`Renderer::FrameProfile`），不是这里猜的。
    const auto& profile = machine.renderer().profile();
    std::printf("画了 %d 帧，%.1f 秒，%.1f fps\n", drawn, seconds,
                seconds > 0.0 ? static_cast<double>(drawn) / seconds : 0.0);
    std::printf("一帧里：prepare %.2f  constants %.2f  stream %.2f  submit %.2f ms\n",
                profile.prepare_ms, profile.constants_ms, profile.stream_ms, profile.submit_ms);
    return drawn < 0 ? 1 : 0;
}
