// 烛龙 (ZhuLong) - 建模器：一个宿主程序，画一份模型，让面板能指着它。
//
// 它是最普通的一种宿主：实现 `Platform`，交出去，剩下的由机器驱动（`System::Run` —— 循环不是
// 宿主的，`Run` 里那句注释就是为这件事写的）。它不组装机器，也不自己管 arena。
//
// 今天它会做这些：
//
//   * 把一份**模型** —— 引擎的 `MeshDescription`，也就是建模器要编辑的那份东西 —— 摆进场景，
//     旁边放一张地面网格和三条轴；
//   * 从面板收鼠标：右键拖着转、中键拖着平移、滚轮缩、左键选一个东西然后拖着它走，**按着 Shift
//     拖则吸附到 0.05 的格子上**；
//   * 选中的东西亮着：面是一层亮色的表面，点是一个小方块；
//   * `1` / `2` 在选面 / 选点之间切，`E` 挤出选中的面，`D` 细分整个模型，`B` 把选中的角切掉，
//     `C` 把选中那个面的一圈边标成硬边（细分时它们不跟着变圆），`X` 删掉选中的面（留一个洞），
//     `Z` 撤销，`S` 存回原处、`O` 从原处再读一遍；编辑器的「上色」按钮给选中的面换一个颜色。
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
// 它还不能：选边、展开 uv。硬边是"整个面的一圈"一次标的（`C`），没有"点中某一条边"这回事；贴图
// 落在 uv 本来就在的地方，没有拖 uv 的地方。
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
#include "zlong/engine/image_file.h"
#include "zlong/engine/mesh.h"
#include "zlong/engine/model_file.h"
#include "zlong/engine/obj_file.h"
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

// 撤销栈最多留多少步。每步是整个模型一份快照 —— 现在是个位数 KB，但模型会长，所以有个上限。
constexpr std::size_t kUndoDepth = 128;

// 每次挤出多少。**定长，不追鼠标** —— 挤完新面是选中的，想放哪儿接着拖就是了（拖动那一套是
// 现成的）。"挤一步再拖"比"挤的时候一路追着指针"好写，也好预料：连按三次就是稳稳的三层。
constexpr float kExtrudeStep = 0.25f;

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

/// 一个单位立方体的**描述**：八个位置、六个四边面。
///
/// 这就是 `MakeBox` 写死在函数体里的那一份，写出来给人看。将来手柄改的就是它里面的一个数字；
/// 现在它是常量，所以六个面每一行的绕序都验过朝外，写在下面。
MeshDescription UnitCube() {
    MeshDescription description;
    // 一种材质，就是原来那个灰。**模型从第一个面起就穿着点什么** —— 拆成几块画的时候不会出现
    // "有一块没有材质"。
    description.materials.push_back({{0.62f, 0.64f, 0.68f, 1.0f}, 0.35f});
    description.positions = {
        {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f},
        {-0.5f, 0.5f, -0.5f},  {-0.5f, 0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {0.5f, 0.5f, -0.5f},
    };

    // 每个面的角按逆时针（从外面看）。
    const std::uint32_t faces[6][4] = {
        {0, 3, 2, 1},   // 下，法线 -Y
        {4, 5, 6, 7},   // 上，+Y
        {1, 2, 6, 5},   // 前，+Z
        {0, 4, 7, 3},   // 后，-Z
        {3, 7, 6, 2},   // 右，+X
        {0, 1, 5, 4},   // 左，-X
    };

    for (const auto& corners : faces) {
        Face face;
        face.count = 4;
        for (int corner = 0; corner < 4; ++corner) {
            face.corners[corner] = corners[corner];
            // uv 铺满这个面，而且要跟着角的顺序走 —— 切线和副切线是从 uv 的斜率推出来的。
            face.uv[corner][0] = (corner == 1 || corner == 2) ? 1.0f : 0.0f;
            face.uv[corner][1] = (corner >= 2) ? 1.0f : 0.0f;
        }
        description.faces.push_back(face);
    }
    return description;
}

/// 建模器本身。它既是那台机器的宿主，也是这个工具的脑子。
class Modeler final : public zlong::system::Platform {
public:
    Modeler(zlong::system::System& machine, const zlong::system::System::Config& config,
            std::uint32_t shadow_size)
        : machine_(machine), config_(config), shadow_size_(shadow_size), model_(UnitCube()) {
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
            }
        }

        for (const zlong::editor::Host::Click& click : editor_->TakeClicks()) {
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
            if (click.button == kLeftButton) {
                if (click.down) {
                    geometry_changed |= Pick(click.x, click.y);
                    BeginDrag(click);
                } else {
                    EndDrag();
                }
            }
        }

        const int notches = editor_->TakeWheel();
        if (notches != 0) {
            // 乘着走而不是加着走：远离和靠近的手感才是对称的，走多远也不会变慢或变抖。
            orbit_.distance *= std::exp(-static_cast<float>(notches) * 0.12f);
            orbit_.distance = std::clamp(orbit_.distance, kNearest, kFarthest);
        }

        const auto pointer = editor_->pointer();
        const int dx = pointer.x - last_pointer_.x;
        const int dy = pointer.y - last_pointer_.y;
        if (dx != 0 || dy != 0) {
            if ((held_ & kRightButton) != 0) {
                orbit_.yaw -= static_cast<float>(dx) * 0.011f;
                orbit_.pitch = std::clamp(orbit_.pitch + static_cast<float>(dy) * 0.011f,
                                          -kPitchLimit, kPitchLimit);
            } else if ((held_ & kMiddleButton) != 0) {
                Pan(dx, dy);
            } else if ((held_ & kLeftButton) != 0) {
                geometry_changed |= Drag();
            }
            last_pointer_ = pointer;
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

    /// 在选什么。点还是面 —— 这是这台工具到今天为止唯一的模式。
    ///
    /// 用 1 / 2 切换，不是 Tab：键盘那根管子只送虚拟键码，**不送修饰键**（见 `viewport.py`），
    /// 所以 Ctrl+Z 到这儿和 Z 长得一样。撤销因此就绑在 Z 上。要把 Shift 吸附或者真正的 Ctrl
    /// 组合加回来，得先给那根管子加一位修饰掩码。
    enum class Mode { Face, Vertex };

    /// 一步撤销要放回去的东西：**模型，和当时选着什么**。
    ///
    /// 只放模型是不够的。挤出和细分都会改面的个数，撤销之后那个号可能指到别处、甚至指到外面去
    /// —— 挤了三层再撤三次，模型回到六个面，而选中还停在 17 号上，`Rebuild` 下一个 `[]` 就是
    /// 读别人的内存。选中是编辑状态的一部分，就该跟着一起回退。
    struct Snapshot {
        MeshDescription model;
        Mode mode = Mode::Face;
        int face = -1;
        int vertex = -1;
    };

    /// 现在的这一份，用来压栈。
    Snapshot Now() const { return Snapshot{model_, mode_, picked_face_, picked_vertex_}; }
    /// 把模型的描述烘一遍，并记下"第几个三角形属于第几个面"。
    ///
    /// 拾取打的是**烘出来的三角形**，不是描述里的多边形 —— 这样点到的一定是画出来的那一片，
    /// 包括四边面是从哪条对角线折开的。要打描述的话，拾取和渲染就各有一套折叠规则了。
    void Rebuild() {
        std::string error;
        if (!zlong::engine::BakeMesh(model_, model_mesh_, error)) {
            std::printf("模型烘不出来：%s\n", error.c_str());
            model_mesh_ = Mesh{};
            triangle_face_.clear();
            return;
        }
        triangle_face_.clear();
        for (std::size_t index = 0; index < model_.faces.size(); ++index) {
            const int triangles = static_cast<int>(model_.faces[index].count) - 2;
            for (int one = 0; one < triangles; ++one) {
                triangle_face_.push_back(static_cast<int>(index));
            }
        }

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
        scene_.AddNode(0, 0, zlong::engine::Translation(0.0f, -0.5f, 0.0f) *
                                 zlong::engine::Scale(12.0f, 1.0f, 12.0f));

        // **模型按材质拆开画。** 引擎那边一个可画节点配一种材质，所以"一个东西上有几种颜色"
        // 在那一侧就是"几个节点" —— 拆在这儿，管线和着色器一个字不用改（见 `SplitByMaterial`）。
        //
        // `model_mesh_`（整个模型烘一份）还是留着：**拾取打的是它**，因为点到的一定是画出来的
        // 那一片，而且这样一来拾取逻辑不用管模型被分成了几块。
        std::map<std::string, std::uint32_t> uploaded;   // 同一个图片文件不重复上传
        for (const zlong::engine::MaterialSplit& split : zlong::engine::SplitByMaterial(model_)) {
            Mesh piece;
            if (!zlong::engine::BakeMesh(split.description, piece, error)) {
                std::printf("模型的某一块烘不出来：%s\n", error.c_str());
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
            scene_.AddNode(mesh, material, Mat4{});
        }

        AddAxes();

        // 选中用的那个亮材质。**它排在最后**，下标是算出来的 —— 模型有几种材质是变的，写死一个
        // 数字的话，模型一上色高亮就会指到别的材质上去。
        zlong::engine::Material hot;
        hot.tint = {1.0f, 0.62f, 0.18f, 1.0f};
        hot.specular = 0.6f;
        hot_material_ = static_cast<std::uint32_t>(scene_.materials.size());
        scene_.materials.push_back(hot);

        // 选中的那个面，单独画一份、沿它自己的法线推出去一丁点。
        //
        // 推这一下是必须的：引擎没有 polygon offset，两片共面的面会打架，谁赢按片段来，看上去
        // 是一条抖动的花边。一个法线上的小位移，是这件事最省的做法。
        // 选中的东西：面就把它单独画一份，点就放一个小方块在它上面。都用那个亮材质（2 号）。
        if (mode_ == Mode::Face && picked_face_ >= 0) {
            MeshDescription one;
            one.positions = model_.positions;
            one.faces.push_back(model_.faces[static_cast<std::size_t>(picked_face_)]);
            Mesh highlight;
            std::string reason;
            if (zlong::engine::BakeMesh(one, highlight, reason) && !highlight.vertices.empty()) {
                const auto& normal = highlight.vertices[0].normal;
                for (auto& vertex : highlight.vertices) {
                    vertex.position[0] += normal[0] * 0.004f;
                    vertex.position[1] += normal[1] * 0.004f;
                    vertex.position[2] += normal[2] * 0.004f;
                }
                scene_.meshes.push_back(highlight);
                const auto node = static_cast<std::uint32_t>(scene_.meshes.size() - 1);
                scene_.AddNode(node, hot_material_, Mat4{});
            }
        }
        if (mode_ == Mode::Vertex && picked_vertex_ >= 0) {
            // 一个小方块骑在那个点上。不用点或者十字：那些要么在 IR 里表达不出来（没有 swizzle），
            // 要么就得给渲染器开一条"编辑器专用"的路 —— 而一个缩放过的立方体是现成的。
            const Vec3& at = model_.positions[static_cast<std::size_t>(picked_vertex_)];
            scene_.meshes.push_back(zlong::engine::MakeBox());
            const auto marker = static_cast<std::uint32_t>(scene_.meshes.size() - 1);
            scene_.AddNode(marker, hot_material_,
                           zlong::engine::Translation(at.x, at.y, at.z) *
                               zlong::engine::Scale(0.07f));
        }

        if (!machine_.Load(scene_)) {
            std::printf("场景过不去：%s\n", machine_.error().c_str());
        }
        Aim();
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

    /// 左键点一下：看模式，找到射线打中的那个面、或者离指针最近的那个顶点。
    ///
    /// 用的是**这一下自己的位置**，不是指针现在在哪 —— 两者可能差一帧的鼠标移动。
    ///
    /// 返回"要不要重新拼场景"。
    bool Pick(int x, int y) {
        return mode_ == Mode::Vertex ? PickVertex(x, y) : PickFace(x, y);
    }

    bool PickFace(int x, int y) {
        Vec3 origin;
        Vec3 direction;
        RayAt(x, y, origin, direction);

        int best_face = -1;
        float best_distance = 0.0f;
        const std::size_t triangles = model_mesh_.indices.size() / 3;
        for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
            const auto at = static_cast<std::size_t>(triangle) * 3;
            const Vec3 a = Position(model_mesh_.vertices[model_mesh_.indices[at + 0]]);
            const Vec3 b = Position(model_mesh_.vertices[model_mesh_.indices[at + 1]]);
            const Vec3 c = Position(model_mesh_.vertices[model_mesh_.indices[at + 2]]);
            float distance = 0.0f;
            if (Hits(origin, direction, a, b, c, distance) &&
                (best_face < 0 || distance < best_distance)) {
                best_face = triangle_face_[triangle];
                best_distance = distance;
            }
        }

        if (best_face == picked_face_) {
            return false;              // 还是那个面，什么都不用重来
        }
        picked_face_ = best_face;

        if (picked_face_ < 0) {
            std::printf("点空了\n");
        } else {
            std::printf("面 %d / 共 %zu 个，距离 %.2f\n", picked_face_,
                        model_.faces.size(), best_distance);
        }
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

        int best = -1;
        float best_depth = 0.0f;
        for (std::size_t index = 0; index < model_.positions.size(); ++index) {
            float x = 0.0f;
            float y = 0.0f;
            if (!ToPixel(model_.positions[index], x, y)) {
                continue;
            }
            const float dx = x - want_x;
            const float dy = y - want_y;
            if (dx * dx + dy * dy > reach) {
                continue;
            }
            const float depth = Dot(Sub(model_.positions[index], eye), forward);
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
            std::printf("顶点 %d / 共 %zu 个\n", picked_vertex_, model_.positions.size());
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
        switch (key) {
        case '1':
            if (mode_ == Mode::Face) {
                return false;
            }
            mode_ = Mode::Face;
            picked_vertex_ = -1;
            std::printf("选面（1 面 / 2 点，Z 撤销）\n");
            return true;
        case '2':
            if (mode_ == Mode::Vertex) {
                return false;
            }
            mode_ = Mode::Vertex;
            picked_face_ = -1;
            std::printf("选点（1 面 / 2 点，Z 撤销）\n");
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
        model_ = std::move(imported);
        // **导进来之后就没有"存到哪儿"了。** 那是别人的文件，按 `S` 不该往它上面写 —— 想留下就
        // 另存一个（「保存」会问），而那正好是"我的模型"和"借来的文件"该有的区别。
        model_path_.clear();
        picked_face_ = -1;
        picked_vertex_ = -1;

        std::printf("导入了 %s\n", path.c_str());
        std::printf("  点 %zu → %zu", report.points_in_file, report.points_kept);
        if (report.points_dropped > 0) {
            std::printf("（%zu 个没被面用到，丢了）", report.points_dropped);
        }
        std::printf("\n  面 %zu → %zu", report.faces_in_file, model_.faces.size());
        if (report.faces_fanned > 0) {
            std::printf("（%zu 个五个角以上的扇成了三角形）", report.faces_fanned);
        }
        std::printf("，平 %zu 滑 %zu\n", report.faces_flat, report.faces_smooth);
        if (report.lines_ignored > 0) {
            std::printf("  跳过 %zu 行用不上的（物体名、材质那些）\n", report.lines_ignored);
        }
        return true;
    }

    /// 存一份到 `path`，并且记住它 —— 之后按 `S` 就是存回同一个地方。
    ///
    /// **返回"画面要不要重来"**，所以导出的是"没有"：存不动作几何。
    void SaveTo(const std::string& path) {
        std::string reason;
        if (!zlong::engine::SaveModel(path, model_, reason)) {
            std::printf("存不下去：%s\n", reason.c_str());
            return;
        }
        model_path_ = path;
        std::printf("存到 %s（面 %zu，位置 %zu）\n", path.c_str(), model_.faces.size(),
                    model_.positions.size());
    }

    /// 从 `path` 读一份进来，并记住它。读成了就换掉手上的模型，**选中一起清掉** ——
    /// 那些号是照着旧模型来的。
    ///
    /// 读之前压一步撤销：载入会把手上这份顶掉，而"挑错了文件"是不该丢掉半天活的。
    bool LoadFrom(const std::string& path) {
        MeshDescription loaded;
        std::string reason;
        if (!zlong::engine::LoadModel(path, loaded, reason)) {
            std::printf("读不进来：%s\n", reason.c_str());
            return false;
        }
        PushUndo(Now());
        model_ = std::move(loaded);
        model_path_ = path;
        picked_face_ = -1;
        picked_vertex_ = -1;
        std::printf("读了 %s：面 %zu，位置 %zu\n", path.c_str(), model_.faces.size(),
                    model_.positions.size());
        return true;
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
        for (std::size_t index = 0; index < model_.textures.size(); ++index) {
            if (model_.textures[index].path == stored) {
                slot = static_cast<std::uint32_t>(index);
                break;
            }
        }
        PushUndo(Now());
        if (slot == zlong::engine::kNoTexture) {
            slot = static_cast<std::uint32_t>(model_.textures.size());
            model_.textures.push_back(zlong::engine::MeshTexture{stored, std::move(image)});
        }
        const std::uint32_t material = model_.faces[static_cast<std::size_t>(picked_face_)].material;
        if (material < model_.materials.size()) {
            model_.materials[material].texture = slot;
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

    /// 给选中的那个面换一个颜色。
    ///
    /// **同一个颜色复用同一个材质槽。** 每次新建一个的话，涂十次就有十种材质，拆出来的网格也跟着
    /// 长 —— 而"两个面是同一种红"在这份数据里本来就该是一件事。
    bool AssignColour(const std::array<float, 4>& tint) {
        if (mode_ != Mode::Face || picked_face_ < 0) {
            std::printf("先选一个面再上色（按 1 切到选面，左键点一个面）\n");
            return false;
        }
        const auto wanted = static_cast<std::uint32_t>(model_.materials.size());
        std::uint32_t slot = wanted;
        for (std::size_t index = 0; index < model_.materials.size(); ++index) {
            const auto& was = model_.materials[index].tint;
            if (was[0] == tint[0] && was[1] == tint[1] && was[2] == tint[2] && was[3] == tint[3]) {
                slot = static_cast<std::uint32_t>(index);
                break;
            }
        }
        if (slot == wanted) {
            zlong::engine::MeshMaterial made;
            made.tint = tint;
            made.specular = model_.materials.empty() ? 0.3f : model_.materials.front().specular;
            model_.materials.push_back(made);
        }
        if (model_.faces[static_cast<std::size_t>(picked_face_)].material == slot) {
            return false;              // 已经是那个颜色了，什么都不用重来
        }
        PushUndo(Now());
        model_.faces[static_cast<std::size_t>(picked_face_)].material = slot;
        std::printf("面 %d 换成材质 %u（%.2f, %.2f, %.2f），模型现在有 %zu 种\n", picked_face_, slot,
                    tint[0], tint[1], tint[2], model_.materials.size());
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
        if (!zlong::engine::ExtrudeFace(model_, static_cast<std::uint32_t>(picked_face_),
                                        kExtrudeStep, edited, reason)) {
            std::printf("挤不出来：%s\n", reason.c_str());
            return false;
        }
        PushUndo(Now());
        model_ = std::move(edited);
        picked_face_ = static_cast<int>(model_.faces.size()) - 1;
        std::printf("挤出 %.2f：面 %zu，位置 %zu，选中 %d\n", kExtrudeStep, model_.faces.size(),
                    model_.positions.size(), picked_face_);
        return true;
    }

    /// Catmull-Clark 细分一次，**整个模型**。
    ///
    /// 面的下标全变了，所以旧的选中没有意义 —— 清掉，别让它指着一个巧合。用户想接着干活就再点
    /// 一下。
    bool SubdivideModel() {
        MeshDescription edited;
        std::string reason;
        if (!zlong::engine::Subdivide(model_, edited, reason)) {
            std::printf("细分不了：%s\n", reason.c_str());
            return false;
        }
        const std::size_t faces_before = model_.faces.size();
        const std::size_t points_before = model_.positions.size();
        PushUndo(Now());
        model_ = std::move(edited);
        mode_ = Mode::Face;
        picked_face_ = -1;
        picked_vertex_ = -1;
        std::printf("细分：面 %zu → %zu，位置 %zu → %zu（选中已清）\n", faces_before,
                    model_.faces.size(), points_before, model_.positions.size());
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
            const Face& face = model_.faces[static_cast<std::size_t>(picked_face_)];
            for (std::uint32_t corner = 0; corner < face.count; ++corner) {
                corners.push_back(face.corners[corner]);
            }
        }

        MeshDescription edited;
        std::string reason;
        if (!zlong::engine::CornerCut(model_, corners, kBevelFraction, edited, reason)) {
            std::printf("切不了：%s\n", reason.c_str());
            return false;
        }
        const std::size_t faces_before = model_.faces.size();
        const std::size_t points_before = model_.positions.size();
        PushUndo(Now());
        model_ = std::move(edited);
        mode_ = Mode::Face;
        picked_face_ = -1;
        picked_vertex_ = -1;
        std::printf("切角 %.0f%%：面 %zu → %zu，位置 %zu → %zu（选中已清）\n",
                    kBevelFraction * 100.0f, faces_before, model_.faces.size(), points_before,
                    model_.positions.size());
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
        const Face& face = model_.faces[static_cast<std::size_t>(picked_face_)];
        std::vector<Crease> ring;
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            const std::uint32_t a = face.corners[corner];
            const std::uint32_t b = face.corners[(corner + 1) % face.count];
            if (a != b) {
                ring.push_back(Crease{a < b ? a : b, a < b ? b : a});
            }
        }

        const auto lookup = [this](const Crease& edge) {
            return std::binary_search(model_.creases.begin(), model_.creases.end(), edge);
        };
        const bool all_hard =
            std::all_of(ring.begin(), ring.end(), lookup);

        PushUndo(Now());
        if (all_hard) {
            std::vector<Crease> kept;
            for (const Crease& edge : model_.creases) {
                if (std::find(ring.begin(), ring.end(), edge) == ring.end()) {
                    kept.push_back(edge);
                }
            }
            model_.creases = std::move(kept);
            std::printf("这个面的 %zu 条边不硬了（还剩 %zu 条硬边）\n", ring.size(),
                        model_.creases.size());
        } else {
            for (const Crease& edge : ring) {
                if (!lookup(edge)) {
                    model_.creases.push_back(edge);
                }
            }
            std::sort(model_.creases.begin(), model_.creases.end());
            std::printf("这个面的 %zu 条边硬了（现在 %zu 条硬边）—— 细分的时候它们不会变圆\n",
                        ring.size(), model_.creases.size());
        }
        return true;
    }

    /// 删掉选中的那个面，**在那儿留一个洞**。
    ///
    /// 这是唯一一个让面**变少**的操作，所以没人指着的点会跟着丢掉、剩下的重排 —— 于是面的下标和点
    /// 的下标都会变，选中照例清掉。
    ///
    /// 只认选面模式。点模式按它是"把这个点周围的面都删了"，那是另一个操作（等于挖一个洞），不是
    /// 用户按一下"删面"想要的东西。
    bool DeleteSelection() {
        if (mode_ != Mode::Face || picked_face_ < 0) {
            std::printf("先选一个面（删面只删面，按 1 切到选面）\n");
            return false;
        }
        MeshDescription edited;
        std::string reason;
        const auto which = static_cast<std::uint32_t>(picked_face_);
        if (!zlong::engine::DeleteFace(model_, which, edited, reason)) {
            std::printf("删不了：%s\n", reason.c_str());
            return false;
        }
        const std::size_t faces_before = model_.faces.size();
        const std::size_t points_before = model_.positions.size();
        PushUndo(Now());
        model_ = std::move(edited);
        picked_face_ = -1;
        picked_vertex_ = -1;
        std::printf("删掉面 %u：面 %zu → %zu，位置 %zu → %zu（选中已清）\n", which, faces_before,
                    model_.faces.size(), points_before, model_.positions.size());
        return true;
    }

    /// 撤销：把上一份快照放回去 —— 模型和选中一起。
    ///
    /// 不试着"反着做"那一步 —— 逆操作要每一个操作都写对，而快照只需要有一次是对的。
    bool Undo() {
        if (undo_.empty()) {
            std::printf("没有可撤销的了\n");
            return false;
        }
        const Snapshot& step = undo_.back();
        model_ = step.model;
        mode_ = step.mode;
        picked_face_ = step.face;
        picked_vertex_ = step.vertex;
        undo_.pop_back();
        std::printf("撤销：面 %zu，位置 %zu，还剩 %zu 步\n", model_.faces.size(),
                    model_.positions.size(), undo_.size());
        return true;
    }

    /// 选中的那个东西的中心 —— 拖动平面要穿过它。
    Vec3 SelectionCentre() const {
        if (mode_ == Mode::Vertex && picked_vertex_ >= 0) {
            return model_.positions[static_cast<std::size_t>(picked_vertex_)];
        }
        Vec3 sum{0.0f, 0.0f, 0.0f};
        if (picked_face_ < 0) {
            return sum;
        }
        const Face& face = model_.faces[static_cast<std::size_t>(picked_face_)];
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            sum = Add(sum, model_.positions[face.corners[corner]]);
        }
        return Mul(sum, 1.0f / static_cast<float>(face.count));
    }

    bool HasSelection() const {
        return (mode_ == Mode::Vertex && picked_vertex_ >= 0) ||
               (mode_ == Mode::Face && picked_face_ >= 0);
    }

    /// 一条射线打在拖动平面上，落在哪。
    bool PlaneHit(const Vec3& origin, const Vec3& direction, const Vec3& normal, Vec3& hit) const {
        const float denominator = Dot(direction, normal);
        if (std::fabs(denominator) < 1e-6f) {
            return false;
        }
        const float along = Dot(Sub(drag_plane_, origin), normal) / denominator;
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
        if (!HasSelection()) {
            return;
        }

        // **挪谁、从哪挪，在这里定下来。** 之后再有人换模式、改选择，都改不了这一拖要对哪些
        // 位置下手；`drag_origins_` 是它们此刻的值，`MoveSelection` 每次从它算起。
        if (mode_ == Mode::Vertex) {
            drag_positions_.push_back(static_cast<std::uint32_t>(picked_vertex_));
        } else {
            const Face& face = model_.faces[static_cast<std::size_t>(picked_face_)];
            for (std::uint32_t corner = 0; corner < face.count; ++corner) {
                drag_positions_.push_back(face.corners[corner]);
            }
        }
        for (const std::uint32_t index : drag_positions_) {
            drag_origins_.push_back(model_.positions[index]);
        }

        Vec3 forward;
        Vec3 right;
        Vec3 up;
        CameraBasis(forward, right, up);
        drag_plane_ = SelectionCentre();

        Vec3 origin;
        Vec3 direction;
        RayAt(click.x, click.y, origin, direction);
        if (!PlaneHit(origin, direction, forward, drag_from_)) {
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
        if (!PlaneHit(origin, direction, forward, now)) {
            return false;
        }
        const Vec3 raw = Sub(now, drag_from_);
        Vec3 delta = raw;
        if (snapping_) {
            // `drag_plane_` 就是按下那一刻选择的中点，所以 `drag_plane_ + raw` 是它现在到的地方。
            const Vec3 landed = Add(drag_plane_, raw);
            delta = Sub(Vec3{Snap(landed.x), Snap(landed.y), Snap(landed.z)}, drag_plane_);
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

    /// 把这一拖抓住的那几个位置挪 `delta`。    ///
    /// **从按下那一刻的值算起**，不是在当前位置上累加：这一函数每帧都被调用，累加会把浮点误差
    /// 攒起来，而且一放一按就该重新起算。
    ///
    /// 面模式下动的自然是它那几个**角**，而角是**位置的下标** —— 所以和邻居共用的那个点会一起
    /// 走。这是焊接那件事的另一面，也是它成为可编辑的基础：不是 bug。
    void MoveSelection(const Vec3& delta) {
        for (std::size_t index = 0; index < drag_positions_.size(); ++index) {
            model_.positions[drag_positions_[index]] = Add(drag_origins_[index], delta);
        }
    }

    /// 这一拖抓住的那些位置，现在在哪 —— 报了它，一次编辑就有了一个能对得上的结果。
    Vec3 MovedCentre() const {
        Vec3 sum{0.0f, 0.0f, 0.0f};
        if (drag_positions_.empty()) {
            return sum;
        }
        for (const std::uint32_t index : drag_positions_) {
            sum = Add(sum, model_.positions[index]);
        }
        return Mul(sum, 1.0f / static_cast<float>(drag_positions_.size()));
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

    MeshDescription model_;
    Mesh model_mesh_;
    std::vector<int> triangle_face_;   // 第几个三角形属于第几个面
    Scene scene_;
    /// 选中用的那个亮材质在 `scene_.materials` 里的下标。**算出来的，不是写死的** —— 模型有几
    /// 种材质是会变的。
    std::uint32_t hot_material_ = 0;
    /// 手上这份模型从哪儿来 / 要存到哪儿。空着就是还没说过 —— 那种时候 `S` / `O` 会明说，
    /// 而不是猜一个地方写下去。
    std::string model_path_;

    Orbit orbit_;
    int held_ = 0;                     // 哪些键正按着，按位
    bool snapping_ = false;            // Shift 有没有按着
    zlong::editor::Host::Pointer last_pointer_;

    Mode mode_ = Mode::Face;
    int picked_face_ = -1;
    int picked_vertex_ = -1;           // model_.positions 的下标

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
