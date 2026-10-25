// 烛龙 (ZhuLong) - 布尔运算：两块几何的并 / 交 / 差。
//
// 和 `ExtrudeFace` / `Subdivide` 一样，是**描述进、描述出**的纯函数 —— 不碰选中、不碰撤销、不碰画面。
//
// **做法是经典的 BSP 分裂**（csg.js 那一套）：把两块各自建成一棵 BSP 树，用对方裁掉自己的面，再拼回
// 来。它**是"能用的近似"**：切出来的面会变成三角形（`Face` 只放得下三或四个角），共面或退化的极端情况
// 可能有瑕疵 —— 对图元拼出来的形状够用。真要工业级的稳健布尔，那是另一个量级（精确谓词加半边结构）。
//
// 两个操作数各带自己的材质和贴图；出来那一份是两边**并起来的**（材质按 tint 去重、贴图按路径去重），
// 所以调用方拿到手就能烘。
//
// 法线方向有个前提：**两个操作数都得是"外面朝外"的**（引擎里烘出来的形状都是）。差集是按这个方向算的。

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "zlong/engine/mesh.h"

namespace zlong::engine {

/// 三种布尔。
enum class BooleanOp {
    Union,       // 并：两块合成一块
    Intersect,   // 交：只留两块重叠的部分
    Difference,  // 差：从 `a` 里挖掉 `b`
};

/// `a op b`，结果写进 `out`。失败（`a` 或 `b` 是空的）时填 `error` 并返回 false。
///
/// 两个操作数**必须已经摆在世界里**（点就是世界坐标）—— 摆放是调用方的事。
bool BooleanMesh(const MeshDescription& a, const MeshDescription& b, BooleanOp op,
                 MeshDescription& out, std::string& error);

namespace detail {

/// BSP 里的一个多边形：点带着它的 uv，外加它所在那个平面和它穿哪种材质。
struct CsgVertex {
    Vec3 position;
    float u = 0.0f;
    float v = 0.0f;
};

struct CsgPolygon {
    std::vector<CsgVertex> vertices;
    Vec3 normal{};
    float offset = 0.0f;          ///< 平面方程 `dot(normal, p) = offset`
    std::uint32_t material = 0;
};

inline constexpr float kCsgEpsilon = 1e-5f;

CsgVertex LerpVertex(const CsgVertex& a, const CsgVertex& b, float t) {
    return CsgVertex{Vec3{a.position.x + (b.position.x - a.position.x) * t,
                          a.position.y + (b.position.y - a.position.y) * t,
                          a.position.z + (b.position.z - a.position.z) * t},
                     a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
}

/// 用平面 `dot(normal, p) = offset` 切 `polygon`，把每一片放进对的那个筐。
void SplitPolygon(const Vec3& plane_normal, float plane_offset, const CsgPolygon& polygon,
                  std::vector<CsgPolygon>& coplanar_front,
                  std::vector<CsgPolygon>& coplanar_back, std::vector<CsgPolygon>& front,
                  std::vector<CsgPolygon>& back) {
    constexpr int kCoplanar = 0;
    constexpr int kFront = 1;
    constexpr int kBack = 2;
    constexpr int kSpanning = 3;

    int polygon_kind = 0;
    std::vector<int> kinds(polygon.vertices.size(), kCoplanar);
    for (std::size_t i = 0; i < polygon.vertices.size(); ++i) {
        const float t = Dot(plane_normal, polygon.vertices[i].position) - plane_offset;
        const int kind = t < -kCsgEpsilon ? kBack : (t > kCsgEpsilon ? kFront : kCoplanar);
        polygon_kind |= kind;
        kinds[i] = kind;
    }

    switch (polygon_kind) {
    case kCoplanar:
        (Dot(plane_normal, polygon.normal) > 0.0f ? coplanar_front : coplanar_back)
            .push_back(polygon);
        break;
    case kFront:
        front.push_back(polygon);
        break;
    case kBack:
        back.push_back(polygon);
        break;
    default: {                     // 跨过这个平面：切开
        std::vector<CsgVertex> f;
        std::vector<CsgVertex> b;
        const std::size_t count = polygon.vertices.size();
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t j = (i + 1) % count;
            const int ki = kinds[i];
            const int kj = kinds[j];
            const CsgVertex& vi = polygon.vertices[i];
            const CsgVertex& vj = polygon.vertices[j];
            if (ki != kBack) {
                f.push_back(vi);
            }
            if (ki != kFront) {
                b.push_back(vi);
            }
            if ((ki | kj) == kSpanning) {
                const float numerator = plane_offset - Dot(plane_normal, vi.position);
                const float denominator =
                    Dot(plane_normal, Vec3{vj.position.x - vi.position.x,
                                           vj.position.y - vi.position.y,
                                           vj.position.z - vi.position.z});
                const float t = std::fabs(denominator) < 1e-20f ? 0.0f : numerator / denominator;
                const CsgVertex middle = LerpVertex(vi, vj, t);
                f.push_back(middle);
                b.push_back(middle);
            }
        }
        if (f.size() >= 3) {
            front.push_back(CsgPolygon{f, polygon.normal, polygon.offset, polygon.material});
        }
        if (b.size() >= 3) {
            back.push_back(CsgPolygon{b, polygon.normal, polygon.offset, polygon.material});
        }
        break;
    }
    }
}

/// 一棵 BSP。`Build` 的时候挑第一个多边形的面当分割面 —— 简单、够用（不做平衡）。
struct CsgNode {
    bool has_plane = false;
    Vec3 normal{};
    float offset = 0.0f;
    std::vector<CsgPolygon> polygons;
    std::unique_ptr<CsgNode> front;
    std::unique_ptr<CsgNode> back;

    void Build(const std::vector<CsgPolygon>& input) {
        if (input.empty()) {
            return;
        }
        if (!has_plane) {
            has_plane = true;
            normal = input.front().normal;
            offset = input.front().offset;
        }
        std::vector<CsgPolygon> front_list;
        std::vector<CsgPolygon> back_list;
        for (const CsgPolygon& polygon : input) {
            SplitPolygon(normal, offset, polygon, polygons, polygons, front_list, back_list);
        }
        if (!front_list.empty()) {
            if (front == nullptr) {
                front = std::make_unique<CsgNode>();
            }
            front->Build(front_list);
        }
        if (!back_list.empty()) {
            if (back == nullptr) {
                back = std::make_unique<CsgNode>();
            }
            back->Build(back_list);
        }
    }

    void Invert() {
        for (CsgPolygon& polygon : polygons) {
            std::reverse(polygon.vertices.begin(), polygon.vertices.end());
            polygon.normal = Vec3{-polygon.normal.x, -polygon.normal.y, -polygon.normal.z};
            polygon.offset = -polygon.offset;
        }
        normal = Vec3{-normal.x, -normal.y, -normal.z};
        offset = -offset;
        if (front != nullptr) {
            front->Invert();
        }
        if (back != nullptr) {
            back->Invert();
        }
        std::swap(front, back);
    }

    std::vector<CsgPolygon> ClipPolygons(const std::vector<CsgPolygon>& input) const {
        if (!has_plane) {
            return input;
        }
        std::vector<CsgPolygon> front_list;
        std::vector<CsgPolygon> back_list;
        for (const CsgPolygon& polygon : input) {
            SplitPolygon(normal, offset, polygon, front_list, back_list, front_list, back_list);
        }
        if (front != nullptr) {
            front_list = front->ClipPolygons(front_list);
        }
        back_list = back != nullptr ? back->ClipPolygons(back_list) : std::vector<CsgPolygon>{};
        front_list.insert(front_list.end(), back_list.begin(), back_list.end());
        return front_list;
    }

    void ClipTo(const CsgNode& other) {
        polygons = other.ClipPolygons(polygons);
        if (front != nullptr) {
            front->ClipTo(other);
        }
        if (back != nullptr) {
            back->ClipTo(other);
        }
    }

    void AllPolygons(std::vector<CsgPolygon>& out) const {
        out.insert(out.end(), polygons.begin(), polygons.end());
        if (front != nullptr) {
            front->AllPolygons(out);
        }
        if (back != nullptr) {
            back->AllPolygons(out);
        }
    }
};

/// 一份描述 -> 一堆 BSP 多边形（每个面一个多边形，法线用 Newell 算）。
void ToPolygons(const MeshDescription& mesh, std::vector<CsgPolygon>& out) {
    for (const Face& face : mesh.faces) {
        float normal[3];
        const float magnitude = detail::NewellNormal(mesh, face, normal);
        if (!(magnitude > 1e-20f)) {
            continue;              // 没面积的面不要
        }
        CsgPolygon polygon;
        polygon.normal = Vec3{normal[0] / magnitude, normal[1] / magnitude, normal[2] / magnitude};
        polygon.offset = Dot(polygon.normal, mesh.positions[face.corners[0]]);
        polygon.material = face.material;
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            polygon.vertices.push_back(CsgVertex{mesh.positions[face.corners[corner]],
                                                 face.uv[corner][0], face.uv[corner][1]});
        }
        out.push_back(std::move(polygon));
    }
}

/// BSP 多边形 -> 一份描述。大于四个角的多边形从第一个角扇成三角形（`Face` 只放得下三或四个角）。
///
/// 出来的点**一个角一个**，最后按小间距并一次 —— 不然描述里的点会多出一大截。
void ToDescription(const std::vector<CsgPolygon>& polygons, MeshDescription& out) {
    for (const CsgPolygon& polygon : polygons) {
        if (polygon.vertices.size() < 3) {
            continue;
        }
        for (std::size_t k = 1; k + 1 < polygon.vertices.size(); ++k) {
            const std::array<const CsgVertex*, 3> corner = {&polygon.vertices[0],
                                                            &polygon.vertices[k],
                                                            &polygon.vertices[k + 1]};
            Face face;
            face.count = 3;
            face.smooth = false;
            face.material = polygon.material;
            for (std::uint32_t c = 0; c < 3; ++c) {
                face.corners[c] = static_cast<std::uint32_t>(out.positions.size());
                face.uv[c][0] = corner[c]->u;
                face.uv[c][1] = corner[c]->v;
                out.positions.push_back(corner[c]->position);
            }
            out.faces.push_back(face);
        }
    }
}

}  // namespace detail

inline bool BooleanMesh(const MeshDescription& a, const MeshDescription& b, BooleanOp op,
                        MeshDescription& out, std::string& error) {
    if (a.faces.empty() || b.faces.empty()) {
        error = "布尔运算要两块都有面";
        return false;
    }

    // 材质和贴图先并起来（材质按 tint 去重、贴图按路径去重），两个操作数的面都换成并表里的号 ——
    // 这样出来的一份只有一个表，调用方拿到就能烘。
    out = MeshDescription{};
    const auto material_slot = [&out](const MeshMaterial& material) {
        for (std::size_t i = 0; i < out.materials.size(); ++i) {
            const auto& was = out.materials[i].tint;
            if (was[0] == material.tint[0] && was[1] == material.tint[1] &&
                was[2] == material.tint[2] && was[3] == material.tint[3]) {
                return static_cast<std::uint32_t>(i);
            }
        }
        out.materials.push_back(material);
        return static_cast<std::uint32_t>(out.materials.size() - 1);
    };

    const auto import_mesh = [&](const MeshDescription& source,
                                 std::vector<detail::CsgPolygon>& into) {
        std::vector<std::uint32_t> texture_slot(source.textures.size(), kNoTexture);
        for (std::size_t t = 0; t < source.textures.size(); ++t) {
            std::uint32_t slot = kNoTexture;
            for (std::size_t u = 0; u < out.textures.size(); ++u) {
                if (out.textures[u].path == source.textures[t].path) {
                    slot = static_cast<std::uint32_t>(u);
                    break;
                }
            }
            if (slot == kNoTexture) {
                slot = static_cast<std::uint32_t>(out.textures.size());
                out.textures.push_back(source.textures[t]);
            }
            texture_slot[t] = slot;
        }
        std::vector<std::uint32_t> material_map(source.materials.size(), 0);
        for (std::size_t m = 0; m < source.materials.size(); ++m) {
            MeshMaterial copy = source.materials[m];
            if (copy.texture != kNoTexture && copy.texture < texture_slot.size()) {
                copy.texture = texture_slot[copy.texture];
            }
            material_map[m] = material_slot(copy);
        }
        MeshDescription remapped = source;
        for (Face& face : remapped.faces) {
            face.material = face.material < material_map.size() ? material_map[face.material] : 0u;
        }
        detail::ToPolygons(remapped, into);
    };

    std::vector<detail::CsgPolygon> polygons_a;
    std::vector<detail::CsgPolygon> polygons_b;
    import_mesh(a, polygons_a);
    import_mesh(b, polygons_b);
    if (polygons_a.empty() || polygons_b.empty()) {
        error = "布尔运算里有一块一个面都没剩下";
        return false;
    }

    detail::CsgNode node_a;
    detail::CsgNode node_b;
    node_a.Build(polygons_a);
    node_b.Build(polygons_b);

    switch (op) {
    case BooleanOp::Union:
        node_a.ClipTo(node_b);
        node_b.ClipTo(node_a);
        node_b.Invert();
        node_b.ClipTo(node_a);
        node_b.Invert();
        {
            std::vector<detail::CsgPolygon> tail;
            node_b.AllPolygons(tail);
            node_a.Build(tail);
        }
        break;
    case BooleanOp::Intersect:
        node_a.Invert();
        node_b.ClipTo(node_a);
        node_b.Invert();
        node_a.ClipTo(node_b);
        node_b.ClipTo(node_a);
        {
            std::vector<detail::CsgPolygon> tail;
            node_b.AllPolygons(tail);
            node_a.Build(tail);
        }
        node_a.Invert();
        break;
    case BooleanOp::Difference:
        node_a.Invert();
        node_a.ClipTo(node_b);
        node_b.ClipTo(node_a);
        node_b.Invert();
        node_b.ClipTo(node_a);
        node_b.Invert();
        {
            std::vector<detail::CsgPolygon> tail;
            node_b.AllPolygons(tail);
            node_a.Build(tail);
        }
        node_a.Invert();
        break;
    }

    std::vector<detail::CsgPolygon> result;
    node_a.AllPolygons(result);
    detail::ToDescription(result, out);
    if (out.faces.empty()) {
        error = "布尔运算没有留下面（可能两块根本不相交）";
        return false;
    }
    // 出来是"一个角一个点"；按小间距并一次，描述才不至于膨胀，也才好接着编辑。
    const std::vector<std::uint32_t> remap = MergeVertices(out.positions, 1e-4f);
    for (Face& face : out.faces) {
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            face.corners[corner] = remap[face.corners[corner]];
        }
    }
    CompressVertices(out);
    error.clear();
    return true;
}

}  // namespace zlong::engine
