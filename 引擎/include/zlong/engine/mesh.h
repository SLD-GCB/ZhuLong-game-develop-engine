// 烛龙 (ZhuLong) - a couple of meshes, generated rather than loaded.
//
// Flat-shaded on purpose: every vertex of a face carries that face's own normal,
// so the interpolated normal across the triangle is exactly that normal and
// keeps its direction. The scene's fragment shader normalizes it anyway, because
// the model matrix scales it.
//
// Winding is counter-clockwise seen from outside, which is what the rasterizer
// treats as front-facing -- `culling_drops_the_winding_that_faces_away` pins that
// convention down, and the scene runs with back faces culled.
//
// uv is a plain 0..1 square per face: enough to see a texture land.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "zlong/engine/math.h"

namespace zlong::engine {

/// A vertex of a procedural mesh.
///
/// The tangent frame is baked rather than derived at shading time: the IR has no
/// swizzle, so a cross product is not expressible there, and carrying the
/// bitangent costs one vertex attribute where computing it would cost a change to
/// the shader VM.
struct MeshVertex {
    float position[4];
    float normal[4];
    float uv[4];
    /// Unit. `tangent x bitangent == normal`, so a tangent-space normal map reads
    /// the way an author expects.
    float tangent[4];
    float bitangent[4];
};

constexpr std::uint32_t kNormalOffset = 16;
constexpr std::uint32_t kUvOffset = 32;
constexpr std::uint32_t kTangentOffset = 48;
constexpr std::uint32_t kBitangentOffset = 64;

struct Mesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint16_t> indices;

    std::uint32_t VertexBytes() const {
        return static_cast<std::uint32_t>(vertices.size() * sizeof(MeshVertex));
    }
    std::uint32_t IndexBytes() const {
        return static_cast<std::uint32_t>(indices.size() * sizeof(std::uint16_t));
    }
};

/// A unit cube centred on the origin. 24 vertices, because each of the six faces
/// needs its own normal.
inline Mesh MakeBox() {
    const float h = 0.5f;
    const float faces[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    // Two tangents per face, ordered so the winding comes out counter-clockwise
    // as seen from outside.
    const float tangents[6][2][3] = {
        {{0, 0, -1}, {0, 1, 0}}, {{0, 0, 1}, {0, 1, 0}}, {{1, 0, 0}, {0, 0, -1}},
        {{1, 0, 0}, {0, 0, 1}},  {{1, 0, 0}, {0, 1, 0}}, {{-1, 0, 0}, {0, 1, 0}},
    };

    Mesh mesh;
    for (int face = 0; face < 6; ++face) {
        const auto base = static_cast<std::uint16_t>(mesh.vertices.size());
        const float* n = faces[face];
        const float* u = tangents[face][0];
        const float* v = tangents[face][1];
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& corner : corners) {
            const float cu = corner[0] * h;
            const float cv = corner[1] * h;
            MeshVertex vertex{};
            for (int axis = 0; axis < 3; ++axis) {
                vertex.position[axis] = n[axis] * h + u[axis] * cu + v[axis] * cv;
                vertex.normal[axis] = n[axis];
                vertex.tangent[axis] = u[axis];
                vertex.bitangent[axis] = v[axis];
            }
            vertex.position[3] = 1.0f;
            // The same corner order, mapped onto the unit square.
            vertex.uv[0] = corner[0] * 0.5f + 0.5f;
            vertex.uv[1] = corner[1] * 0.5f + 0.5f;
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(),
                            {static_cast<std::uint16_t>(base + 0), static_cast<std::uint16_t>(base + 1),
                             static_cast<std::uint16_t>(base + 2), static_cast<std::uint16_t>(base + 0),
                             static_cast<std::uint16_t>(base + 2), static_cast<std::uint16_t>(base + 3)});
    }
    return mesh;
}

namespace detail {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 6.28318530717958647692f;

inline void Normalize3(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length <= 0.0f) {
        v[0] = 0.0f;
        v[1] = 1.0f;
        v[2] = 0.0f;
        return;
    }
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
}

/// Fill a vertex's frame from its position, a normal and a tangent. The bitangent
/// is derived so `tangent x bitangent == normal` holds, which is the convention the
/// baked frames of MakeBox/MakeGrid follow and what a normal map assumes.
inline void FillFrame(MeshVertex& vertex, const float normal[3], const float tangent[3]) {
    for (int axis = 0; axis < 3; ++axis) {
        vertex.normal[axis] = normal[axis];
        vertex.tangent[axis] = tangent[axis];
    }
    float bitangent[3] = {normal[1] * tangent[2] - normal[2] * tangent[1],
                          normal[2] * tangent[0] - normal[0] * tangent[2],
                          normal[0] * tangent[1] - normal[1] * tangent[0]};
    Normalize3(bitangent);
    for (int axis = 0; axis < 3; ++axis) {
        vertex.bitangent[axis] = bitangent[axis];
    }
}

inline MeshVertex VertexAt(float x, float y, float z, const float normal[3],
                           const float tangent[3], float u, float v) {
    MeshVertex vertex{};
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    vertex.position[3] = 1.0f;
    FillFrame(vertex, normal, tangent);
    vertex.uv[0] = u;
    vertex.uv[1] = v;
    return vertex;
}

inline void Triangle(Mesh& mesh, std::uint16_t a, std::uint16_t b, std::uint16_t c) {
    mesh.indices.insert(mesh.indices.end(), {a, b, c});
}

}  // namespace detail

/// Append one flat-shaded polygon (3 or 4 corners, counter-clockwise seen from
/// outside). The normal, tangent and bitangent all come from the corners, so the
/// frame is consistent by construction.
inline void AddPolygon(Mesh& mesh, const float corners[4][3], int count) {
    const auto base = static_cast<std::uint16_t>(mesh.vertices.size());
    float tangent[3] = {corners[1][0] - corners[0][0], corners[1][1] - corners[0][1],
                        corners[1][2] - corners[0][2]};
    float bitangent[3] = {corners[count - 1][0] - corners[0][0],
                          corners[count - 1][1] - corners[0][1],
                          corners[count - 1][2] - corners[0][2]};
    float normal[3] = {tangent[1] * bitangent[2] - tangent[2] * bitangent[1],
                       tangent[2] * bitangent[0] - tangent[0] * bitangent[2],
                       tangent[0] * bitangent[1] - tangent[1] * bitangent[0]};
    detail::Normalize3(normal);
    detail::Normalize3(tangent);
    for (int corner = 0; corner < count; ++corner) {
        const float u = corner == 0 ? 0.0f : (corner == 1 ? 1.0f : 0.5f);
        const float v = corner == 0 ? 0.0f : (corner == 1 ? 0.0f : 1.0f);
        mesh.vertices.push_back(detail::VertexAt(corners[corner][0], corners[corner][1],
                                                 corners[corner][2], normal, tangent, u, v));
    }
    if (count == 3) {
        detail::Triangle(mesh, base, static_cast<std::uint16_t>(base + 1),
                         static_cast<std::uint16_t>(base + 2));
    } else {
        detail::Triangle(mesh, base, static_cast<std::uint16_t>(base + 1),
                         static_cast<std::uint16_t>(base + 2));
        detail::Triangle(mesh, base, static_cast<std::uint16_t>(base + 2),
                         static_cast<std::uint16_t>(base + 3));
    }
}

/// A unit cylinder: radius 0.5, height 1, centred on the origin, axis +Y.
///
/// The side is smooth-shaded -- one vertex per ring position, shared by the two
/// faces that meet there -- so it reads as round rather than as a prism, which is
/// the whole reason these exist alongside MakeBox. The caps are flat.
inline Mesh MakeCylinder(int sides = 16) {
    const int n = sides < 3 ? 3 : sides;
    const float radius = 0.5f;
    const float half = 0.5f;
    Mesh mesh;

    const auto ring = static_cast<std::uint16_t>(mesh.vertices.size());
    for (int i = 0; i <= n; ++i) {
        const float theta = detail::kTwoPi * static_cast<float>(i) / static_cast<float>(n);
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        const float normal[3] = {c, 0.0f, s};
        const float tangent[3] = {-s, 0.0f, c};
        const float u = static_cast<float>(i) / static_cast<float>(n);
        mesh.vertices.push_back(detail::VertexAt(radius * c, -half, radius * s, normal, tangent, u, 0.0f));
        mesh.vertices.push_back(detail::VertexAt(radius * c, half, radius * s, normal, tangent, u, 1.0f));
    }
    for (int i = 0; i < n; ++i) {
        const auto a = static_cast<std::uint16_t>(ring + i * 2);
        const auto b = static_cast<std::uint16_t>(a + 1);
        const auto c = static_cast<std::uint16_t>(a + 2);
        const auto d = static_cast<std::uint16_t>(a + 3);
        detail::Triangle(mesh, a, b, d);
        detail::Triangle(mesh, a, d, c);
    }

    // Caps: a fan around a centre vertex, wound so the normal looks out of the end.
    for (int cap = 0; cap < 2; ++cap) {
        const float y = cap == 0 ? -half : half;
        const float normal[3] = {0.0f, cap == 0 ? -1.0f : 1.0f, 0.0f};
        const float tangent[3] = {1.0f, 0.0f, 0.0f};
        const auto centre = static_cast<std::uint16_t>(mesh.vertices.size());
        mesh.vertices.push_back(detail::VertexAt(0.0f, y, 0.0f, normal, tangent, 0.5f, 0.5f));
        const auto rim = static_cast<std::uint16_t>(mesh.vertices.size());
        for (int i = 0; i <= n; ++i) {
            const float theta = detail::kTwoPi * static_cast<float>(i) / static_cast<float>(n);
            const float c = std::cos(theta);
            const float s = std::sin(theta);
            mesh.vertices.push_back(detail::VertexAt(radius * c, y, radius * s, normal, tangent,
                                                     c * 0.5f + 0.5f, s * 0.5f + 0.5f));
        }
        for (int i = 0; i < n; ++i) {
            const auto p = static_cast<std::uint16_t>(rim + i);
            const auto q = static_cast<std::uint16_t>(rim + i + 1);
            if (cap == 0) {
                detail::Triangle(mesh, centre, p, q);
            } else {
                detail::Triangle(mesh, centre, q, p);
            }
        }
    }
    return mesh;
}

/// A unit cone: base radius 0.5 at y = -0.5, apex at y = +0.5, axis +Y. Smooth
/// sides, flat base.
inline Mesh MakeCone(int sides = 16) {
    const int n = sides < 3 ? 3 : sides;
    const float radius = 0.5f;
    const float half = 0.5f;
    Mesh mesh;

    // The outward normal of a cone's side tilts up by the base-radius:height ratio.
    const float slope = radius / (2.0f * half);
    const auto ring = static_cast<std::uint16_t>(mesh.vertices.size());
    for (int i = 0; i <= n; ++i) {
        const float theta = detail::kTwoPi * static_cast<float>(i) / static_cast<float>(n);
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        float normal[3] = {c, slope, s};
        detail::Normalize3(normal);
        const float tangent[3] = {-s, 0.0f, c};
        mesh.vertices.push_back(detail::VertexAt(radius * c, -half, radius * s, normal, tangent,
                                                 static_cast<float>(i) / static_cast<float>(n), 0.0f));
    }
    const float apex_normal[3] = {0.0f, 1.0f, 0.0f};
    const float apex_tangent[3] = {1.0f, 0.0f, 0.0f};
    const auto apex = static_cast<std::uint16_t>(mesh.vertices.size());
    mesh.vertices.push_back(detail::VertexAt(0.0f, half, 0.0f, apex_normal, apex_tangent, 0.5f, 1.0f));

    for (int i = 0; i < n; ++i) {
        detail::Triangle(mesh, apex, static_cast<std::uint16_t>(ring + i + 1),
                         static_cast<std::uint16_t>(ring + i));
    }

    // Base cap.
    const float base_normal[3] = {0.0f, -1.0f, 0.0f};
    const float base_tangent[3] = {1.0f, 0.0f, 0.0f};
    const auto centre = static_cast<std::uint16_t>(mesh.vertices.size());
    mesh.vertices.push_back(detail::VertexAt(0.0f, -half, 0.0f, base_normal, base_tangent, 0.5f, 0.5f));
    const auto rim = static_cast<std::uint16_t>(mesh.vertices.size());
    for (int i = 0; i <= n; ++i) {
        const float theta = detail::kTwoPi * static_cast<float>(i) / static_cast<float>(n);
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        mesh.vertices.push_back(detail::VertexAt(radius * c, -half, radius * s, base_normal,
                                                 base_tangent, c * 0.5f + 0.5f, s * 0.5f + 0.5f));
    }
    for (int i = 0; i < n; ++i) {
        detail::Triangle(mesh, centre, static_cast<std::uint16_t>(rim + i),
                         static_cast<std::uint16_t>(rim + i + 1));
    }
    return mesh;
}

/// A unit sphere: radius 0.5, centred on the origin, smooth-shaded. `rings` is the
/// number of latitude bands, `segments` the longitude ones.
inline Mesh MakeSphere(int rings = 8, int segments = 16) {
    const int bands = rings < 2 ? 2 : rings;
    const int segments_count = segments < 3 ? 3 : segments;
    const float radius = 0.5f;
    Mesh mesh;

    for (int i = 0; i <= bands; ++i) {
        const float phi = detail::kPi * static_cast<float>(i) / static_cast<float>(bands);
        const float y = radius * std::cos(phi);
        const float ring = radius * std::sin(phi);
        for (int j = 0; j <= segments_count; ++j) {
            const float theta =
                detail::kTwoPi * static_cast<float>(j) / static_cast<float>(segments_count);
            const float c = std::cos(theta);
            const float s = std::sin(theta);
            const float normal[3] = {ring * c / radius, y / radius, ring * s / radius};
            const float tangent[3] = {-s, 0.0f, c};
            mesh.vertices.push_back(detail::VertexAt(ring * c, y, ring * s, normal, tangent,
                                                     static_cast<float>(j) /
                                                         static_cast<float>(segments_count),
                                                     1.0f - static_cast<float>(i) /
                                                                 static_cast<float>(bands)));
        }
    }
    const auto stride = static_cast<std::uint16_t>(segments_count + 1);
    for (int i = 0; i < bands; ++i) {
        for (int j = 0; j < segments_count; ++j) {
            const auto a = static_cast<std::uint16_t>(i * stride + j);
            const auto b = static_cast<std::uint16_t>(a + 1);
            const auto c = static_cast<std::uint16_t>(a + stride);
            const auto d = static_cast<std::uint16_t>(c + 1);
            detail::Triangle(mesh, a, b, d);
            detail::Triangle(mesh, a, d, c);
        }
    }
    return mesh;
}

/// A flat circular face in the XZ plane, normal +Y, radius 0.5, centred on the origin.
///
/// Its uv is the bounding square -- `uv = position + 0.5` -- so a square texture laid on it
/// lands with the texture's centre in the middle of the disc. That is what makes it the
/// face of a chess piece: the character goes in the middle of the square, and the four
/// corners fall outside the disc and are never sampled.
inline Mesh MakeDisc(int sides = 32) {
    const int n = sides < 3 ? 3 : sides;
    const float radius = 0.5f;
    Mesh mesh;

    MeshVertex centre{};
    centre.position[3] = 1.0f;
    centre.normal[1] = 1.0f;
    centre.tangent[0] = 1.0f;
    centre.bitangent[2] = -1.0f;  // (1,0,0) x (0,0,-1) = (0,1,0), the normal
    centre.uv[0] = 0.5f;
    centre.uv[1] = 0.5f;
    mesh.vertices.push_back(centre);

    for (int i = 0; i <= n; ++i) {
        const float theta = detail::kTwoPi * static_cast<float>(i) / static_cast<float>(n);
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        MeshVertex vertex{};
        vertex.position[0] = radius * c;
        vertex.position[2] = radius * s;
        vertex.position[3] = 1.0f;
        vertex.normal[1] = 1.0f;
        vertex.tangent[0] = 1.0f;
        vertex.bitangent[2] = -1.0f;
        vertex.uv[0] = radius * c + 0.5f;
        vertex.uv[1] = radius * s + 0.5f;
        mesh.vertices.push_back(vertex);
    }
    // Wound so the fan faces +Y, the same way MakeCylinder's top cap is.
    for (int i = 0; i < n; ++i) {
        detail::Triangle(mesh, 0, static_cast<std::uint16_t>(2 + i),
                         static_cast<std::uint16_t>(1 + i));
    }
    return mesh;
}

/// A unit quad in the XZ plane, normal +Y, cut into `divisions` x `divisions`
/// cells. Scale it up to make ground.
///
/// Subdivision is optional now that the rasterizer clips to the near plane; it
/// was the workaround that kept the ground visible while it did not. One cell is
/// a plain quad, which is what a flat ground actually is.
///
/// `uv_tiles` repeats the texture across the quad: the uv runs 0..uv_tiles instead of 0..1,
/// so a floor thirty units across lays its boards down at a sane size rather than stretching
/// one tile over the whole thing. The sampler repeats, so this is the one knob that matters
/// for a surface larger than its texture.
inline Mesh MakeGrid(int divisions, float uv_tiles = 1.0f) {
    const int n = divisions < 1 ? 1 : divisions;
    Mesh mesh;
    for (int iz = 0; iz <= n; ++iz) {
        for (int ix = 0; ix <= n; ++ix) {
            const float u = static_cast<float>(ix) / static_cast<float>(n);
            const float v = static_cast<float>(iz) / static_cast<float>(n);
            MeshVertex vertex{};
            vertex.position[0] = u - 0.5f;
            vertex.position[1] = 0.0f;
            vertex.position[2] = v - 0.5f;
            vertex.position[3] = 1.0f;
            vertex.normal[1] = 1.0f;
            // tangent +X and bitangent -Z, chosen so tangent x bitangent is +Y.
            vertex.tangent[0] = 1.0f;
            vertex.bitangent[2] = -1.0f;
            vertex.uv[0] = u * uv_tiles;
            vertex.uv[1] = v * uv_tiles;
            mesh.vertices.push_back(vertex);
        }
    }
    for (int iz = 0; iz < n; ++iz) {
        for (int ix = 0; ix < n; ++ix) {
            const auto a = static_cast<std::uint16_t>(iz * (n + 1) + ix);
            const auto b = static_cast<std::uint16_t>(a + 1);
            const auto c = static_cast<std::uint16_t>(a + n + 1);
            const auto d = static_cast<std::uint16_t>(c + 1);
            // Wound so the surface normal comes out +Y, matching the vertex
            // normals. The other order gives (b - a) x (d - a) = (0, -1, 0),
            // which faces down -- invisible with culling off, and the whole
            // ground vanished the moment it was switched on.
            mesh.indices.insert(mesh.indices.end(), {a, d, b, a, c, d});
        }
    }
    return mesh;
}

// --- authoring a mesh ------------------------------------------------------------------------
//
// Everything above is a bake with its description written into the function body: a cube is a
// description, and MakeBox is that description compiled. The two types below are the same idea
// with the description handed in -- which is what a modeler edits while the user drags something,
// and what an importer produces after reading a file. Between them they are the one way into
// geometry that is not one of the built-in shapes.
//
// The description is **welded**: one entry per position in space, and a face's corners point into
// it. That is what makes the shape editable -- dragging a corner is changing one number, and every
// face that shares it follows. A Mesh is the opposite: one vertex per *corner*, so each corner can
// carry its own normal and its own uv. BakeMesh is the step between the two, and it is the reason
// a caller never has to think about splitting a seam.

/// 没有贴图的那个下标。**它住在这一层**（而不是 `scene.h` 里），因为现在模型那边的材质也有贴图
/// 了，而 `scene.h` 是包含 `mesh.h` 的 —— 反过来放会绕成一个圈。同一个名字，两边是同一个常量。
inline constexpr std::uint32_t kNoTexture = 0xFFFF'FFFFu;

/// RGBA8 texels, tight, top row first.
///
/// 一份像素，和它从哪儿来无关：可能是程序里造的（`MakeCheckerTexture`），也可能是读来的
/// （`LoadBmp`）。所以它住在网格这一层，而"场景里有几张"那件事在 `scene.h` 里。
struct Texture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;

    bool Valid() const {
        return width != 0 && height != 0 &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
};

/// 一个模型里的一种材质 —— 一个颜色、一个高光强度，和一张可有可无的贴图。
///
/// **它属于模型，不属于 `Mesh`。** `Mesh` 是几何（点和面），材质是"这些面看起来是什么"。场景那一侧
/// 有它自己的 `Material`（那个还带法线贴图），这一份是**模型文件里的说法**，两者靠 `SplitByMaterial`
/// 和工具去接。
struct MeshMaterial {
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    float specular = 0.25f;
    /// 贴图下标，指向 `MeshDescription::textures`；`kNoTexture` 就是没有。
    std::uint32_t texture = kNoTexture;
};

/// 一份贴图，和它是从哪个文件来的。
struct MeshTexture {
    /// 读出这份像素的那个文件，**相对这份 `.model`**。空 = 程序里造出来的，存不回去。
    ///
    /// 路径留着不是为了好看：存回去的时候得写出它，否则存一次贴图就没了。
    std::string path;
    Texture image;
};

/// 一条**硬边**：细分的时候它不跟着变圆。
///
/// 存成一对位置下标，**小的在前** —— 和 `detail::EdgeKey` 一个写法。一份焊接的描述里"这两个点之间
/// 有一条边"是几何事实，没什么好记的；要记下来的那件事是"这条边是硬的"。
///
/// 一条边没有别的属性可记（它是两个面共用出来的，不是谁写下来的），所以硬边是**单独一张表**，
/// 而不是挂在面或者角上。`MeshDescription::creases` 按 `(low, high)` 排好序，于是两张同样的表一定
/// 长得一样。
struct Crease {
    std::uint32_t low = 0;
    std::uint32_t high = 0;

    bool operator<(const Crease& other) const {
        return low != other.low ? low < other.low : high < other.high;
    }
    bool operator==(const Crease& other) const {
        return low == other.low && high == other.high;
    }
};

/// One face: three or four corners, each a position index and a uv.
struct Face {
    /// Indices into `MeshDescription::positions`.
    std::uint32_t corners[4] = {0, 0, 0, 0};
    /// One uv per corner, in the same order as `corners`.
    float uv[4][2] = {};
    /// How many of the four corners are used -- three or four. A four is turned into two
    /// triangles along its shorter diagonal.
    std::uint32_t count = 3;
    /// Average the normal over the faces that meet at each corner instead of using this face's
    /// own. Flat, the default, is what the built-in primitives are and what a hard edge wants.
    bool smooth = false;
    /// Which of `MeshDescription::materials` this face wears.
    std::uint32_t material = 0;
};

/// A mesh being authored: welded positions, faces that point into them, the materials those
/// faces wear, and the textures those materials sample.
struct MeshDescription {
    std::vector<Vec3> positions;
    std::vector<Face> faces;
    /// At least one, always -- a face with no material still has to wear something. The reader
    /// puts a plain white one there when a file does not say.
    std::vector<MeshMaterial> materials;
    /// 材质可以指到的贴图。**已经在内存里了**：读文件的那一步就把它们读出来了，所以工具和场景
    /// 拿到手就能用，不用各自再去找一遍文件。
    std::vector<MeshTexture> textures;
    /// 硬边，按 `(low, high)` 排好序。
    std::vector<Crease> creases;
};

/// Compile a description into the `Mesh` a scene holds.
///
/// **This is the one door into geometry that is not one of the eight built-in shapes**, and both
/// of the things that produce a mesh come through it: a modeler handing over what the user just
/// dragged, and an importer handing over what it read out of a file. That is why it lives here
/// rather than in either of them -- what it applies (which way a face faces, what a smooth corner
/// averages to, how the tangent frame is derived, why the indices are 16-bit) are the engine's
/// rules, and there is exactly one copy of them.
///
/// Returns false and fills `error` when the description cannot become a mesh: a corner pointing
/// outside `positions`, a face that is not three or four corners, a face with no area. A caller
/// that was handed a file needs to know which of those it was.
bool BakeMesh(const MeshDescription& description, Mesh& out, std::string& error);

/// 把一个面沿它自己的法线挤出一段，四周补上侧壁 —— 描述进，描述出。
///
/// **这两个是这台机器上第一批改拓扑的操作**：到今天为止的每一步编辑都只是把点挪个位置，点的
/// 个数和面的个数从来没变过。挤出多出 4 个点和 4 个面，细分把每个面变成 4 个 —— 所以它们不能
/// 长得像"改一个数"，它们造新的。
///
/// 那个被挤的面**会被换掉**（不是留着）：它原来的位置变成侧壁的底边，再留一片在那里就是一块
/// 内部墙 —— 一个封闭的东西里不该有墙。想往里挤就给一个负的距离，侧壁的绕序会跟着翻，法线
/// 仍然朝外。
///
/// 出来的面按 [其余的面…, 侧壁（按角的顺序）, 顶面] 排。**顶面在最后**是个约定：调用方挤完
/// 通常想选中它，那是 `out.faces.size() - 1`。
bool ExtrudeFace(const MeshDescription& description, std::uint32_t face, float distance,
                 MeshDescription& out, std::string& error);

/// Catmull-Clark 细分一次：每个面一个中心点、每条边一个点、每个老顶点按邻域挪一下，然后每个
/// n 边面拆成 n 个四边面。
///
/// 这是"盒子变球"那一步 —— 它**把棱角磨掉**，所以一个立方体细分成一次之后会缩进去、变圆。
///
/// 收敛到极限曲面靠的是那条顶点规则：`(F + 2R + (n-3)P) / n`，F 是邻接面的中心点平均、R 是
/// 邻接边中点的平均、n 是邻接边的条数。**边界上的顶点不动** —— 一条边只挨着一个面的时候，
/// 上面那条规则不适用，而"把它钉在原地"至少是可预期的：开口的形状不会自己飘走。
///
/// `description.creases` 里那些硬边不跟这个规则走，它们是另一条路：
///   * 硬边的**边点在它的中点**，不是那四个点（两个端点加两个面心）的平均，于是边细分完仍然是直的；
///   * 一个点上连着**三条或更多**硬边 = 一个尖角，钉住；正好**两条** = 沿这两条硬边走折线规则
///     `(6P + Q_before + Q_after) / 8`，于是硬边连成的线细分完还是一条硬边（只是自己成了一条平滑
///     的曲线）；**一条或零条**就走上面那条平滑规则 —— 孤零零一条硬边撑不住一个角，它就当作普通边。
///   * 一条硬边细分完变成两条半截的硬边，所以 `out.creases` 是这么续下去的。
///
/// 一份没声明任何硬边的描述，走的还是原来那条路，一个数都没变。
bool Subdivide(const MeshDescription& description, MeshDescription& out, std::string& error);

/// 切角（顶点倒角）：把点到的那些顶点，各用一小片平面切掉。
///
/// 和 `ExtrudeFace`/`Subdivide` 一样改拓扑，而且**造点也造面**。做法是：每个被切的顶点 `V`，
/// 在它每条边 `(V, N)` 上、离 `V` 有 `fraction` 那么远的地方放一个新点；原来面里 `V` 这个角
/// **换成那两个新点**（进边一个、出边一个），角就被削平了；再给 `V` 补一片**封口**面，那片就是
/// 切出来的那块平面。
///
/// 切点是**按"谁朝谁"共用的**：一条边 `(V, W)`，`V` 被切就在 `V` 那头来一个、`W` 被切就在 `W`
/// 那头来一个，两个都切就是两个不同的点，但每一个都是位置表里的同一项 —— 两边共这条边的面看到
/// 的是同一个点，切完仍然严丝合缝，不会裂开。
///
/// 一个角一削，面就未必是三四个角了（三角形三个角都削是六边形），而 `Face` 只放得下四个角，
/// 所以**五个角以上的面从它的中心扇成三角形** —— 中心落在那些角的平均上，都在原来那张面上，形状
/// 一点没变，只是多几个三角形。封口面同理。
///
/// `fraction` 是相对边长的一段，**只收 (0, 0.5)**：0 什么也没切；0.5 会让一条边两头的切点重合、
/// 挤出一片零面积的面。一个顶点如果落在边界上（有边只挨着一个面），它切出来的不是封口是一道豁
/// 口，所以**整个操作拒绝执行**并把那个点报出来。
bool CornerCut(const MeshDescription& description, const std::vector<std::uint32_t>& vertices,
               float fraction, MeshDescription& out, std::string& error);

/// 删掉一个面，**在那儿留一个洞**。
///
/// 这是唯一一个**把面变少**的操作（挤出、细分、切角都只会多），所以它和它们不一样：面少了一个，
/// 那几个角可能就再没有别的面指着了。**没人指着的点丢掉、剩下的重排** —— 和 `obj_file` 导进来时
/// 一个规矩，一份描述里不该留着一个谁也不指的点。
///
/// 于是**面的下标和点的下标都会变**，调用方手里的选中没有意义了（工具那边一概清掉）。
///
/// 硬边跟着一起收：两头有一头没了，或者这条边再没有面用到，那条硬边就一起删 —— 留着它，文件里就
/// 会长出一堆指着空气的 `crease`。
///
/// 删的是最后一个面就**不做**：一份没有面的描述烘不出网格，那不是"删掉一个面"，是把模型清了。
bool DeleteFace(const MeshDescription& description, std::uint32_t face, MeshDescription& out,
                std::string& error);

/// 一份材质，和只穿那种材质的那些面。
struct MaterialSplit {
    MeshMaterial material;
    MeshDescription description;
};

/// 按材质把一份描述拆开，**这是"逐面材质"落地的地方**。
///
/// 引擎那边一个可画节点配一种材质（`Node::material`），所以"一个东西上有两种颜色"在那一侧就是
/// "两个节点"。拆在工具和场景这一层，**管线和着色器一个字都不用改** —— 而另一条路（给顶点加一条
/// 材质索引属性）要动 Maxwell 那条机器字的路。
///
/// 每份只留它那些面，点按用到的重排；一种材质都没用到的不会出现在结果里。描述里一个材质都没声明
/// 的时候就当作只有一份默认的 —— 调用方因此永远拿到至少一项。
std::vector<MaterialSplit> SplitByMaterial(const MeshDescription& description);

namespace detail {

/// A face's normal, by Newell's method, and how long the sum was before it was normalized.
///
/// One rule for three corners and for four, and it stays sane when a quad is not quite planar,
/// which is the ordinary case for anything a person has been dragging. It returns the magnitude
/// rather than normalizing, because this file's `Normalize3` answers a zero-length vector with +Y
/// -- which would quietly turn a sliver of a face into one pointing at the sky.
inline float NewellNormal(const MeshDescription& description, const Face& face, float normal[3]) {
    normal[0] = 0.0f;
    normal[1] = 0.0f;
    normal[2] = 0.0f;
    for (std::uint32_t corner = 0; corner < face.count; ++corner) {
        const Vec3& a = description.positions[face.corners[corner]];
        const Vec3& b = description.positions[face.corners[(corner + 1) % face.count]];
        normal[0] += (a.y - b.y) * (a.z + b.z);
        normal[1] += (a.z - b.z) * (a.x + b.x);
        normal[2] += (a.x - b.x) * (a.y + b.y);
    }
    return std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
}

/// How the uv axes lean in space, read off the face's first triangle. Zero when the uvs have no
/// spread there -- all three corners on the same uv, or all three on one line -- which is a corner
/// nobody has unwrapped yet, and is what `CornerTangent` falls back on.
///
/// A quad's second triangle is not consulted: a face is taken to be flat and its unwrap continuous
/// across it, which is what turning it into a fan assumes too.
inline void UvGradient(const MeshDescription& description, const Face& face, float tangent[3]) {
    tangent[0] = 0.0f;
    tangent[1] = 0.0f;
    tangent[2] = 0.0f;

    const Vec3& p0 = description.positions[face.corners[0]];
    const Vec3& p1 = description.positions[face.corners[1]];
    const Vec3& p2 = description.positions[face.corners[2]];
    const float du1 = face.uv[1][0] - face.uv[0][0];
    const float dv1 = face.uv[1][1] - face.uv[0][1];
    const float du2 = face.uv[2][0] - face.uv[0][0];
    const float dv2 = face.uv[2][1] - face.uv[0][1];

    const float determinant = du1 * dv2 - du2 * dv1;
    if (std::fabs(determinant) < 1e-12f) {
        return;
    }
    const float scale = 1.0f / determinant;
    const float e1[3] = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
    const float e2[3] = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
    for (int axis = 0; axis < 3; ++axis) {
        tangent[axis] = (e1[axis] * dv2 - e2[axis] * dv1) * scale;
    }
}

/// Any direction perpendicular to `normal`, for when there is nothing better. The axis the normal
/// leans on least is the one crossed with it, so the result is never near-parallel to the normal
/// and never near zero.
inline void AnyTangent(const float normal[3], float tangent[3]) {
    if (std::fabs(normal[0]) <= std::fabs(normal[1]) &&
        std::fabs(normal[0]) <= std::fabs(normal[2])) {
        tangent[0] = 0.0f;
        tangent[1] = -normal[2];
        tangent[2] = normal[1];
    } else if (std::fabs(normal[1]) <= std::fabs(normal[2])) {
        tangent[0] = normal[2];
        tangent[1] = 0.0f;
        tangent[2] = -normal[0];
    } else {
        tangent[0] = -normal[1];
        tangent[1] = normal[0];
        tangent[2] = 0.0f;
    }
    Normalize3(tangent);
}

/// The tangent a corner's frame gets: the uv gradient with the part along that corner's normal
/// taken out, so the frame is **orthonormal**. The shader transforms the normal map through it,
/// and a tangent leaning into the normal would shear the result -- `FillFrame` derives the
/// bitangent as `normal x tangent`, which only carries the frame if the two are perpendicular.
inline void CornerTangent(const float gradient[3], const float normal[3], float tangent[3]) {
    const float along = gradient[0] * normal[0] + gradient[1] * normal[1] + gradient[2] * normal[2];
    tangent[0] = gradient[0] - normal[0] * along;
    tangent[1] = gradient[1] - normal[1] * along;
    tangent[2] = gradient[2] - normal[2] * along;

    const float length = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] +
                                   tangent[2] * tangent[2]);
    if (!(length > 1e-6f)) {
        AnyTangent(normal, tangent);
        return;
    }
    for (int axis = 0; axis < 3; ++axis) {
        tangent[axis] /= length;
    }
}

/// A smooth corner's normal: the sum of the faces that meet there, normalized. Falls back to the
/// face's own normal when the sum cancels -- which is what a fin made of two faces back to back
/// does, and it would otherwise be a zero-length normal.
inline void CornerNormal(const float sum[3], const float face_normal[3], float normal[3]) {
    const float length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    if (!(length > 1e-6f)) {
        for (int axis = 0; axis < 3; ++axis) {
            normal[axis] = face_normal[axis];
        }
        return;
    }
    for (int axis = 0; axis < 3; ++axis) {
        normal[axis] = sum[axis] / length;
    }
}

}  // namespace detail

inline bool BakeMesh(const MeshDescription& description, Mesh& out, std::string& error) {
    out = Mesh{};
    if (description.faces.empty()) {
        error = "the description has no faces";
        return false;
    }

    // --- one pass to check the description and collect the normals ---------------------------
    //
    // Face normals and the sum of them at each position, because a smooth corner needs every
    // face that meets there and those can come later in the list than the corner does.
    const std::size_t face_count = description.faces.size();
    std::vector<float> face_normals(face_count * 3, 0.0f);
    std::vector<float> position_sums(description.positions.size() * 3, 0.0f);

    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        if (face.count < 3 || face.count > 4) {
            error = "face " + std::to_string(index) + " has " + std::to_string(face.count) +
                    " corners; a face is three or four";
            return false;
        }
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            if (face.corners[corner] >= description.positions.size()) {
                error = "face " + std::to_string(index) + " corner " + std::to_string(corner) +
                        " is position " + std::to_string(face.corners[corner]) + ", and there are " +
                        std::to_string(description.positions.size());
                return false;
            }
        }

        float normal[3];
        const float magnitude = detail::NewellNormal(description, face, normal);
        // A face with no area has no direction -- not +Y, not anything. Say so, rather than baking
        // a face that points wherever a fallback happened to put it.
        if (!(magnitude > 1e-20f)) {
            error = "face " + std::to_string(index) + " has no area";
            return false;
        }
        for (int axis = 0; axis < 3; ++axis) {
            normal[axis] /= magnitude;
            face_normals[index * 3 + axis] = normal[axis];
        }

        // Unweighted on purpose: every face meeting at a corner has an equal say whatever its
        // area. That is a rule a caller can predict without reading this loop, which is the same
        // reason the sound renderer's rolloff is a straight line.
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            const std::size_t base = static_cast<std::size_t>(face.corners[corner]) * 3;
            for (int axis = 0; axis < 3; ++axis) {
                position_sums[base + axis] += normal[axis];
            }
        }
    }

    // --- one vertex per corner, then the indices ---------------------------------------------
    //
    // One vertex per *corner*, never per position: that is the whole difference between the two
    // types. It is what lets a corner carry the face's own normal (a hard edge) and its own uv --
    // and it is why nothing here has to look for a seam to split at, because every corner is
    // already its own vertex.
    std::size_t corners_used = 0;
    for (const Face& face : description.faces) {
        corners_used += face.count;
    }
    if (corners_used > 0xFFFFu) {
        error = "the description needs " + std::to_string(corners_used) +
                " vertices, and a Mesh index is 16-bit";
        return false;
    }
    out.vertices.reserve(corners_used);

    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        const float* face_normal = &face_normals[index * 3];

        float gradient[3];
        detail::UvGradient(description, face, gradient);

        const auto first = static_cast<std::uint16_t>(out.vertices.size());
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            const Vec3& position = description.positions[face.corners[corner]];

            float normal[3];
            if (face.smooth) {
                detail::CornerNormal(&position_sums[static_cast<std::size_t>(face.corners[corner]) * 3],
                                     face_normal, normal);
            } else {
                for (int axis = 0; axis < 3; ++axis) {
                    normal[axis] = face_normal[axis];
                }
            }

            float tangent[3];
            detail::CornerTangent(gradient, normal, tangent);

            MeshVertex vertex{};
            vertex.position[0] = position.x;
            vertex.position[1] = position.y;
            vertex.position[2] = position.z;
            vertex.position[3] = 1.0f;
            detail::FillFrame(vertex, normal, tangent);
            vertex.uv[0] = face.uv[corner][0];
            vertex.uv[1] = face.uv[corner][1];
            out.vertices.push_back(vertex);
        }

        if (face.count == 3) {
            detail::Triangle(out, first, static_cast<std::uint16_t>(first + 1),
                             static_cast<std::uint16_t>(first + 2));
            continue;
        }

        // A quad becomes two triangles along **the shorter diagonal**. A quad that is not quite
        // planar folds along one of them, and folding along the shorter one moves the surface
        // less -- and on a square, where they are equal, this picks the same pair the grid
        // primitive does, which is what makes the two agree vertex for vertex on a flat square.
        const Vec3& p0 = description.positions[face.corners[0]];
        const Vec3& p1 = description.positions[face.corners[1]];
        const Vec3& p2 = description.positions[face.corners[2]];
        const Vec3& p3 = description.positions[face.corners[3]];
        const auto squared = [](const Vec3& a, const Vec3& b) {
            const float x = a.x - b.x;
            const float y = a.y - b.y;
            const float z = a.z - b.z;
            return x * x + y * y + z * z;
        };
        if (squared(p0, p2) <= squared(p1, p3)) {
            detail::Triangle(out, first, static_cast<std::uint16_t>(first + 1),
                             static_cast<std::uint16_t>(first + 2));
            detail::Triangle(out, first, static_cast<std::uint16_t>(first + 2),
                             static_cast<std::uint16_t>(first + 3));
        } else {
            detail::Triangle(out, first, static_cast<std::uint16_t>(first + 1),
                             static_cast<std::uint16_t>(first + 3));
            detail::Triangle(out, static_cast<std::uint16_t>(first + 1),
                             static_cast<std::uint16_t>(first + 2),
                             static_cast<std::uint16_t>(first + 3));
        }
    }

    error.clear();
    return true;
}

namespace detail {

// 取平均是这两个操作里出现最多的动作，摊开来写会淹掉真正在说什么的那几行。
inline Vec3 Sum(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Times(const Vec3& v, float factor) {
    return {v.x * factor, v.y * factor, v.z * factor};
}
inline Vec3 Average(const Vec3& a, const Vec3& b) { return Times(Sum(a, b), 0.5f); }

/// 一条边，认得出是同一条。
///
/// 两个端点下标，**小的在前** —— 一条边写起来有两种顺序，归一化之后只有一种。这件事在这两个
/// 操作里都要紧：`MeshDescription` 是**焊接**的，"这两个点之间有一条边"是几何事实，不是谁写
/// 下来的，所以凡是需要知道边的地方都得自己先数一遍。
struct EdgeKey {
    std::uint32_t low = 0;
    std::uint32_t high = 0;

    static EdgeKey Of(std::uint32_t a, std::uint32_t b) {
        return a <= b ? EdgeKey{a, b} : EdgeKey{b, a};
    }
    bool operator<(const EdgeKey& other) const {
        return low != other.low ? low < other.low : high < other.high;
    }
};

/// 描述里的面是不是都能用：三个或四个角，而且角都指着存在的点。
///
/// 两个操作各自要用到角上的点，所以都得先问这一句 —— 越界读在这里不是"编不过"，是读一段别人
/// 的内存然后算出一个看着像模像样的法线。
inline bool CornersUsable(const MeshDescription& description, std::size_t index,
                          std::uint32_t corners, std::string& error) {
    if (corners < 3 || corners > 4) {
        error = "face " + std::to_string(index) + " has " + std::to_string(corners) +
                " corners; a face is three or four";
        return false;
    }
    return true;
}

inline bool PointsExist(const MeshDescription& description, std::size_t index, const Face& face,
                        std::string& error) {
    for (std::uint32_t corner = 0; corner < face.count; ++corner) {
        if (face.corners[corner] >= description.positions.size()) {
            error = "face " + std::to_string(index) + " corner " + std::to_string(corner) +
                    " is position " + std::to_string(face.corners[corner]) + ", and there are " +
                    std::to_string(description.positions.size());
            return false;
        }
    }
    return true;
}

}  // namespace detail

inline bool ExtrudeFace(const MeshDescription& description, std::uint32_t face, float distance,
                        MeshDescription& out, std::string& error) {
    out = MeshDescription{};
    if (face >= description.faces.size()) {
        error = "face " + std::to_string(face) + " is past the " +
                std::to_string(description.faces.size()) + " there are";
        return false;
    }
    const Face bottom = description.faces[face];
    if (!detail::CornersUsable(description, face, bottom.count, error) ||
        !detail::PointsExist(description, face, bottom, error)) {
        return false;
    }

    float normal[3];
    const float magnitude = detail::NewellNormal(description, bottom, normal);
    if (!(magnitude > 1e-20f)) {
        error = "face " + std::to_string(face) + " has no area, so it has no direction to go in";
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        normal[axis] /= magnitude;
    }

    out.positions = description.positions;
    const auto lifted = static_cast<std::uint32_t>(out.positions.size());
    for (std::uint32_t corner = 0; corner < bottom.count; ++corner) {
        const Vec3& at = description.positions[bottom.corners[corner]];
        out.positions.push_back(Vec3{at.x + normal[0] * distance, at.y + normal[1] * distance,
                                     at.z + normal[2] * distance});
    }

    // 除了这一个，其余原样留下。
    for (std::uint32_t index = 0; index < description.faces.size(); ++index) {
        if (index != face) {
            out.faces.push_back(description.faces[index]);
        }
    }

    // 侧壁，每条边一片。绕序 `(旧 a, 旧 b, 新 b, 新 a)` 从外面看是逆时针 —— 和这个引擎别处的
    // 面一样。往里挤的时候翻过来，否则法线就朝着里面了。
    for (std::uint32_t corner = 0; corner < bottom.count; ++corner) {
        const std::uint32_t next = (corner + 1) % bottom.count;
        Face wall;
        wall.count = 4;
        // 侧壁穿的是**被挤那个面**的颜色 —— 挤出来的是一截同一个东西，不是另一块料。
        wall.material = bottom.material;
        wall.corners[0] = bottom.corners[corner];
        wall.corners[1] = bottom.corners[next];
        wall.corners[2] = lifted + next;
        wall.corners[3] = lifted + corner;
        if (distance < 0.0f) {
            std::swap(wall.corners[1], wall.corners[3]);
        }
        wall.uv[0][0] = 0.0f; wall.uv[0][1] = 0.0f;
        wall.uv[1][0] = 1.0f; wall.uv[1][1] = 0.0f;
        wall.uv[2][0] = 1.0f; wall.uv[2][1] = 1.0f;
        wall.uv[3][0] = 0.0f; wall.uv[3][1] = 1.0f;
        out.faces.push_back(wall);
    }

    // 顶面：绕序、uv、平还是滑，都跟着被顶掉的那一片。
    Face top = bottom;
    for (std::uint32_t corner = 0; corner < top.count; ++corner) {
        top.corners[corner] = lifted + corner;
    }
    out.faces.push_back(top);

    error.clear();
    return true;
}

inline bool Subdivide(const MeshDescription& description, MeshDescription& out, std::string& error) {
    out = MeshDescription{};
    if (description.faces.empty()) {
        error = "the description has no faces";
        return false;
    }

    const std::size_t face_count = description.faces.size();
    const std::size_t position_count = description.positions.size();
    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        if (!detail::CornersUsable(description, index, face.count, error) ||
            !detail::PointsExist(description, index, face, error)) {
            return false;
        }
    }

    // 先数一遍边：**焊接意味着"边"不是你写下来的，是数出来的。** 同一条边会被两个面各提一次
    // （边界上只提一次），这里就是把它归到一起的那一步。
    std::map<detail::EdgeKey, std::vector<std::uint32_t>> faces_of_edge;
    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            const auto key = detail::EdgeKey::Of(face.corners[corner],
                                                 face.corners[(corner + 1) % face.count]);
            faces_of_edge[key].push_back(static_cast<std::uint32_t>(index));
        }
    }

    // 硬边下面每一步都要问一次（每条边问一遍、每个点把它挨着的边都问一遍），所以先摊成一个集合
    // 查。表里存的是两个下标，而这里手上拿着的是 `EdgeKey`，转一下最直接。
    std::set<detail::EdgeKey> creased_edges;
    for (const Crease& crease : description.creases) {
        if (crease.low != crease.high) {
            creased_edges.insert(detail::EdgeKey::Of(crease.low, crease.high));
        }
    }

    // 新的点按这个顺序排：**每个面的中心、每条边的点、每个老顶点挪完的位置**。顺序就是下标，
    // 下面造面的时候靠那三个基准去指它们。
    std::vector<Vec3> points;
    points.reserve(position_count + face_count + faces_of_edge.size());

    std::vector<Vec3> face_centres(face_count);
    std::vector<std::array<float, 2>> face_uv(face_count);

    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        Vec3 sum{0.0f, 0.0f, 0.0f};
        float u = 0.0f;
        float v = 0.0f;
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            sum = detail::Sum(sum, description.positions[face.corners[corner]]);
            u += face.uv[corner][0];
            v += face.uv[corner][1];
        }
        const float count = static_cast<float>(face.count);
        face_centres[index] = detail::Times(sum, 1.0f / count);
        face_uv[index] = {u / count, v / count};
        points.push_back(face_centres[index]);
    }

    std::map<detail::EdgeKey, std::uint32_t> edge_point;
    for (const auto& entry : faces_of_edge) {
        const detail::EdgeKey& key = entry.first;
        const std::vector<std::uint32_t>& sides = entry.second;
        const Vec3& a = description.positions[key.low];
        const Vec3& b = description.positions[key.high];
        Vec3 at = detail::Average(a, b);            // 边界边、硬边：中点就够了
        if (sides.size() == 2 && creased_edges.count(key) == 0) {
            at = detail::Times(detail::Sum(detail::Sum(a, b),
                                           detail::Sum(face_centres[sides[0]],
                                                       face_centres[sides[1]])),
                               0.25f);
        }
        edge_point[key] = static_cast<std::uint32_t>(points.size());
        points.push_back(at);
    }

    // 每个点挨着哪些边、哪些面。用集合，因为同一个面里的两条边会在它的同一个角上被提两次。
    std::vector<std::set<detail::EdgeKey>> edges_at(position_count);
    std::vector<std::set<std::uint32_t>> faces_at(position_count);
    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            const std::uint32_t here = face.corners[corner];
            const std::uint32_t next = face.corners[(corner + 1) % face.count];
            const std::uint32_t before =
                face.corners[(corner + face.count - 1) % face.count];
            faces_at[here].insert(static_cast<std::uint32_t>(index));
            edges_at[here].insert(detail::EdgeKey::Of(here, next));
            edges_at[here].insert(detail::EdgeKey::Of(here, before));
        }
    }

    const auto moved = static_cast<std::uint32_t>(points.size());
    for (std::size_t index = 0; index < position_count; ++index) {
        const Vec3& here = description.positions[index];
        if (edges_at[index].empty()) {
            points.push_back(here);                 // 没有任何面用到它，原样带着
            continue;
        }
        bool on_boundary = false;
        std::vector<detail::EdgeKey> sharp;         // 这个点上连着哪几条硬边
        for (const detail::EdgeKey& edge : edges_at[index]) {
            if (faces_of_edge[edge].size() != 2) {
                on_boundary = true;
            }
            if (creased_edges.count(edge) != 0) {
                sharp.push_back(edge);
            }
        }
        const float valence = static_cast<float>(edges_at[index].size());
        if (on_boundary || valence < 3.0f) {
            points.push_back(here);
            continue;
        }

        // 三条或更多硬边交汇是个尖角，钉住；正好两条沿硬边走折线规则。一条或零条落到下面的平滑
        // 规则 —— 一条硬边撑不住一个角。
        if (sharp.size() >= 3) {
            points.push_back(here);
            continue;
        }
        if (sharp.size() == 2) {
            const auto self = static_cast<std::uint32_t>(index);
            const Vec3& before = description.positions[sharp[0].low == self ? sharp[0].high
                                                                           : sharp[0].low];
            const Vec3& after = description.positions[sharp[1].low == self ? sharp[1].high
                                                                          : sharp[1].low];
            // (6P + Q_before + Q_after) / 8
            points.push_back(detail::Times(
                detail::Sum(detail::Times(here, 6.0f), detail::Sum(before, after)), 0.125f));
            continue;
        }

        Vec3 around_faces{0.0f, 0.0f, 0.0f};
        for (const std::uint32_t side : faces_at[index]) {
            around_faces = detail::Sum(around_faces, face_centres[side]);
        }
        around_faces = detail::Times(around_faces, 1.0f / static_cast<float>(faces_at[index].size()));

        Vec3 around_edges{0.0f, 0.0f, 0.0f};
        for (const detail::EdgeKey& edge : edges_at[index]) {
            around_edges = detail::Sum(
                around_edges,
                detail::Average(description.positions[edge.low], description.positions[edge.high]));
        }
        around_edges = detail::Times(around_edges, 1.0f / valence);

        // (F + 2R + (n-3)P) / n
        points.push_back(detail::Times(
            detail::Sum(detail::Sum(around_faces, detail::Times(around_edges, 2.0f)),
                        detail::Times(here, valence - 3.0f)),
            1.0f / valence));
    }

    // 每个 n 边面拆成 n 个四边面：角 → 出边的点 → 面的中心 → 入边的点。**绕着走的方向和原来
    // 一致**，所以拆完法线还是朝外。
    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        const auto centre = static_cast<std::uint32_t>(index);
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            const std::uint32_t next_corner = (corner + 1) % face.count;
            const std::uint32_t before = (corner + face.count - 1) % face.count;
            const std::uint32_t here = face.corners[corner];

            Face quad;
            quad.count = 4;
            quad.corners[0] = moved + here;
            quad.corners[1] = edge_point[detail::EdgeKey::Of(here, face.corners[next_corner])];
            quad.corners[2] = centre;
            quad.corners[3] = edge_point[detail::EdgeKey::Of(face.corners[before], here)];

            const float u0 = face.uv[corner][0];
            const float v0 = face.uv[corner][1];
            const float un = face.uv[next_corner][0];
            const float vn = face.uv[next_corner][1];
            const float ub = face.uv[before][0];
            const float vb = face.uv[before][1];
            quad.uv[0][0] = u0;             quad.uv[0][1] = v0;
            quad.uv[1][0] = 0.5f * (u0 + un); quad.uv[1][1] = 0.5f * (v0 + vn);
            quad.uv[2][0] = face_uv[index][0]; quad.uv[2][1] = face_uv[index][1];
            quad.uv[3][0] = 0.5f * (ub + u0); quad.uv[3][1] = 0.5f * (vb + v0);
            quad.smooth = face.smooth;
            quad.material = face.material;
            out.faces.push_back(quad);
        }
    }

    // 一条硬边拆成两条半截的硬边：老顶点 → 边点，边点 → 老顶点。边点在这条边的中点上（上面那条
    // 规则），所以两半还在原来那条直线上，硬边就这么续到了下一层。
    for (const Crease& crease : description.creases) {
        if (crease.low >= position_count || crease.high >= position_count ||
            crease.low == crease.high) {
            continue;   // 指着不存在点的硬边：丢掉，别让下游去解一个越界的下标
        }
        const auto found = edge_point.find(detail::EdgeKey::Of(crease.low, crease.high));
        if (found == edge_point.end()) {
            continue;   // 这条边上一个面都没有，细分之后它也就不存在了
        }
        const auto key = detail::EdgeKey::Of(crease.low, crease.high);
        const std::uint32_t middle = found->second;
        out.creases.push_back(Crease{moved + key.low, middle});
        out.creases.push_back(Crease{moved + key.high, middle});
    }
    std::sort(out.creases.begin(), out.creases.end());

    out.positions = std::move(points);
    error.clear();
    return true;
}

inline bool CornerCut(const MeshDescription& description,
                      const std::vector<std::uint32_t>& vertices, float fraction,
                      MeshDescription& out, std::string& error) {
    out = MeshDescription{};
    if (description.faces.empty()) {
        error = "the description has no faces";
        return false;
    }
    if (!(fraction > 0.0f) || fraction >= 0.5f) {
        error = "the cut has to be some way into the edge, up to but not including half";
        return false;
    }

    const std::size_t face_count = description.faces.size();
    const std::size_t position_count = description.positions.size();
    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        if (!detail::CornersUsable(description, index, face.count, error) ||
            !detail::PointsExist(description, index, face, error)) {
            return false;
        }
    }

    // 要切哪些点。重的一律当一次；"没有面用到它"的也当没说要切 —— 那周围什么也没有。
    std::vector<bool> cut(position_count, false);
    for (const std::uint32_t vertex : vertices) {
        if (vertex < position_count) {
            cut[vertex] = true;
        }
    }
    std::vector<std::set<std::uint32_t>> faces_at(position_count);
    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            faces_at[face.corners[corner]].insert(static_cast<std::uint32_t>(index));
        }
    }
    for (std::size_t index = 0; index < position_count; ++index) {
        if (faces_at[index].empty()) {
            cut[index] = false;
        }
    }

    // 老点、老材质、老贴图原样带着 —— 面的角还指着老点，只有被切的那几个角换成新点。
    out.positions = description.positions;
    out.materials = description.materials;
    out.textures = description.textures;

    // 切点：键是**有序的** `(谁, 朝谁)`。两个方向是两个点，所以不能像 `EdgeKey` 那样排序。
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> cut_point_of;
    const auto cut_point = [&](std::uint32_t self, std::uint32_t other) -> std::uint32_t {
        const auto key = std::make_pair(self, other);
        const auto found = cut_point_of.find(key);
        if (found != cut_point_of.end()) {
            return found->second;
        }
        const Vec3& from = description.positions[self];
        const Vec3& to = description.positions[other];
        const auto index = static_cast<std::uint32_t>(out.positions.size());
        out.positions.push_back(detail::Sum(from, detail::Times(to - from, fraction)));
        cut_point_of[key] = index;
        return index;
    };

    // **五个角以上的面从中心扇成三角形。** 一个角一削面就可能超过四个角，而 `Face` 只放得下四个；
    // 中心落在这些角的平均上，都在原来那张（平的）面上，所以形状没有变。
    const auto emit = [&](const std::vector<std::uint32_t>& poly,
                          const std::vector<std::array<float, 2>>& uv, std::uint32_t material,
                          bool smooth) {
        if (poly.size() <= 4) {
            Face face;
            face.count = static_cast<std::uint32_t>(poly.size());
            for (std::size_t k = 0; k < poly.size(); ++k) {
                face.corners[k] = poly[k];
                face.uv[k][0] = uv[k][0];
                face.uv[k][1] = uv[k][1];
            }
            face.smooth = smooth;
            face.material = material;
            out.faces.push_back(face);
            return;
        }
        Vec3 centre{0.0f, 0.0f, 0.0f};
        float cu = 0.0f;
        float cv = 0.0f;
        for (std::size_t k = 0; k < poly.size(); ++k) {
            centre = detail::Sum(centre, out.positions[poly[k]]);
            cu += uv[k][0];
            cv += uv[k][1];
        }
        const float many = static_cast<float>(poly.size());
        centre = detail::Times(centre, 1.0f / many);
        cu /= many;
        cv /= many;
        const auto middle = static_cast<std::uint32_t>(out.positions.size());
        out.positions.push_back(centre);
        for (std::size_t k = 0; k < poly.size(); ++k) {
            const std::size_t next = (k + 1) % poly.size();
            Face face;
            face.count = 3;
            face.corners[0] = middle;
            face.corners[1] = poly[k];
            face.corners[2] = poly[next];
            face.uv[0][0] = cu;        face.uv[0][1] = cv;
            face.uv[1][0] = uv[k][0];  face.uv[1][1] = uv[k][1];
            face.uv[2][0] = uv[next][0]; face.uv[2][1] = uv[next][1];
            face.smooth = smooth;
            face.material = material;
            out.faces.push_back(face);
        }
    };

    // ① 老面削角：进边的切点、出边的切点，替掉原来那个角。**顺序和原来一致**，法线也就还朝外。
    std::vector<std::uint32_t> poly;
    std::vector<std::array<float, 2>> uv;
    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& face = description.faces[index];
        poly.clear();
        uv.clear();
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            const std::uint32_t here = face.corners[corner];
            if (!cut[here]) {
                poly.push_back(here);
                uv.push_back({face.uv[corner][0], face.uv[corner][1]});
                continue;
            }
            const std::uint32_t next = face.corners[(corner + 1) % face.count];
            const std::uint32_t before = face.corners[(corner + face.count - 1) % face.count];
            const std::uint32_t next_corner = (corner + 1) % face.count;
            const std::uint32_t before_corner = (corner + face.count - 1) % face.count;
            const float back = 1.0f - fraction;
            // 先补进边那头的切点，再补出边那头的 —— 顺着走的方向。
            poly.push_back(cut_point(here, before));
            uv.push_back({face.uv[corner][0] * back + face.uv[before_corner][0] * fraction,
                          face.uv[corner][1] * back + face.uv[before_corner][1] * fraction});
            poly.push_back(cut_point(here, next));
            uv.push_back({face.uv[corner][0] * back + face.uv[next_corner][0] * fraction,
                          face.uv[corner][1] * back + face.uv[next_corner][1] * fraction});
        }
        emit(poly, uv, face.material, face.smooth);
    }

    // ② 每个被切的点补一片封口。**绕圈走**：这一点上每个面都有"进边"和"出边"，一个面的进边正是
    // 前一个面的出边，顺着这个就能把一圈面排出来，切点跟着排出来。走不回起点（点在边上）就报错
    // —— 边上切出来的不是一片封口，是一道豁口。
    for (std::size_t index = 0; index < position_count; ++index) {
        if (!cut[index]) {
            continue;
        }
        const auto self = static_cast<std::uint32_t>(index);
        const std::vector<std::uint32_t> around(faces_at[index].begin(), faces_at[index].end());

        std::map<std::uint32_t, std::uint32_t> outgoing;    // 面 → 出边那头的另一个端点
        std::map<std::uint32_t, std::uint32_t> incoming;    // 面 → 进边那头的另一个端点
        std::map<std::uint32_t, std::uint32_t> by_outgoing; // 出边那头 → 面
        for (const std::uint32_t side : around) {
            const Face& face = description.faces[side];
            for (std::uint32_t corner = 0; corner < face.count; ++corner) {
                if (face.corners[corner] != self) {
                    continue;
                }
                const std::uint32_t next = face.corners[(corner + 1) % face.count];
                const std::uint32_t before = face.corners[(corner + face.count - 1) % face.count];
                outgoing[side] = next;
                incoming[side] = before;
                by_outgoing[next] = side;
                break;
            }
        }

        std::vector<std::uint32_t> ring;
        std::uint32_t walk = around[0];
        bool closed = around.size() >= 3;
        for (std::size_t step = 0; closed && step < around.size(); ++step) {
            ring.push_back(outgoing[walk]);
            const auto next = by_outgoing.find(incoming[walk]);
            if (next == by_outgoing.end()) {
                closed = false;
                break;
            }
            walk = next->second;
        }
        if (!closed || walk != around[0] || ring.size() != around.size()) {
            error = "vertex " + std::to_string(self) + " is on an edge -- the cut would leave a hole";
            return false;
        }

        std::vector<std::uint32_t> cap;
        cap.reserve(ring.size());
        for (const std::uint32_t other : ring) {
            cap.push_back(cut_point(self, other));
        }
        // 朝向：封口该朝着这个角的外面。拿它自己的法线比一个相邻面，反了就翻过来。绕着走的那套
        // 规则理论上已经对了，但"理论上"在这儿不值一次返工。
        float normal[3] = {0.0f, 0.0f, 0.0f};
        for (std::size_t k = 0; k < cap.size(); ++k) {
            const Vec3& a = out.positions[cap[k]];
            const Vec3& b = out.positions[cap[(k + 1) % cap.size()]];
            normal[0] += (a.y - b.y) * (a.z + b.z);
            normal[1] += (a.z - b.z) * (a.x + b.x);
            normal[2] += (a.x - b.x) * (a.y + b.y);
        }
        float neighbour[3];
        detail::NewellNormal(description, description.faces[around[0]], neighbour);
        if (normal[0] * neighbour[0] + normal[1] * neighbour[1] + normal[2] * neighbour[2] < 0.0f) {
            std::reverse(cap.begin(), cap.end());
        }

        // 封口是**新的一小片**，给它一圈绕着走的 uv：贴图落上去是圆的、居中，不是一片花。
        std::vector<std::array<float, 2>> cap_uv(cap.size());
        for (std::size_t k = 0; k < cap.size(); ++k) {
            const float angle = 6.2831853f * static_cast<float>(k) / static_cast<float>(cap.size());
            cap_uv[k] = {0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::sin(angle)};
        }
        emit(cap, cap_uv, description.faces[around[0]].material, false);
    }

    // ③ 硬边跟着收：一条硬边 `(a, b)` 切完之后是它两头剩下的那段。两头都没切就原样。
    std::set<detail::EdgeKey> edges;
    for (const Face& face : description.faces) {
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            edges.insert(detail::EdgeKey::Of(face.corners[corner],
                                             face.corners[(corner + 1) % face.count]));
        }
    }
    for (const Crease& crease : description.creases) {
        if (crease.low >= position_count || crease.high >= position_count ||
            crease.low == crease.high) {
            continue;
        }
        const auto key = detail::EdgeKey::Of(crease.low, crease.high);
        if (edges.count(key) == 0) {
            continue;
        }
        const std::uint32_t low = cut[key.low] ? cut_point(key.low, key.high) : key.low;
        const std::uint32_t high = cut[key.high] ? cut_point(key.high, key.low) : key.high;
        if (low == high) {
            continue;
        }
        out.creases.push_back(Crease{low < high ? low : high, low < high ? high : low});
    }
    std::sort(out.creases.begin(), out.creases.end());
    out.creases.erase(std::unique(out.creases.begin(), out.creases.end()), out.creases.end());

    error.clear();
    return true;
}

inline bool DeleteFace(const MeshDescription& description, std::uint32_t face,
                       MeshDescription& out, std::string& error) {
    out = MeshDescription{};
    if (description.faces.empty()) {
        error = "the description has no faces";
        return false;
    }

    const std::size_t face_count = description.faces.size();
    for (std::size_t index = 0; index < face_count; ++index) {
        const Face& one = description.faces[index];
        if (!detail::CornersUsable(description, index, one.count, error) ||
            !detail::PointsExist(description, index, one, error)) {
            return false;
        }
    }
    if (face >= face_count) {
        error = "there is no face " + std::to_string(face) + " -- there are " +
                std::to_string(face_count);
        return false;
    }
    if (face_count == 1) {
        error = "that is the only face, and a model with no faces cannot be baked";
        return false;
    }

    // 留住的面按原来的顺序，只少一个 —— 顺序不变，剩下的东西看着还是原来的样子。
    for (std::size_t index = 0; index < face_count; ++index) {
        if (index != face) {
            out.faces.push_back(description.faces[index]);
        }
    }

    // 还有哪些点被用到。**按用到的先后重排**（不是按老下标）—— 和 `obj_file` 一个规矩。
    constexpr std::uint32_t kUnused = 0xFFFF'FFFFu;
    std::vector<std::uint32_t> moved(description.positions.size(), kUnused);
    for (Face& one : out.faces) {
        for (std::uint32_t corner = 0; corner < one.count; ++corner) {
            const std::uint32_t old = one.corners[corner];
            if (moved[old] == kUnused) {
                moved[old] = static_cast<std::uint32_t>(out.positions.size());
                out.positions.push_back(description.positions[old]);
            }
            one.corners[corner] = moved[old];
        }
    }

    // 硬边：两头都还在、而且这条边还有面用到，才跟着走。
    std::set<detail::EdgeKey> edges;
    for (const Face& one : out.faces) {
        for (std::uint32_t corner = 0; corner < one.count; ++corner) {
            edges.insert(detail::EdgeKey::Of(one.corners[corner],
                                             one.corners[(corner + 1) % one.count]));
        }
    }
    for (const Crease& crease : description.creases) {
        if (crease.low >= moved.size() || crease.high >= moved.size()) {
            continue;
        }
        const std::uint32_t low = moved[crease.low];
        const std::uint32_t high = moved[crease.high];
        if (low == kUnused || high == kUnused || low == high) {
            continue;               // 有一头没人用了，这条边也就没有了
        }
        if (edges.count(detail::EdgeKey::Of(low, high)) == 0) {
            continue;               // 这条边一个面都不挨着了
        }
        out.creases.push_back(Crease{low < high ? low : high, low < high ? high : low});
    }
    std::sort(out.creases.begin(), out.creases.end());
    out.creases.erase(std::unique(out.creases.begin(), out.creases.end()), out.creases.end());

    // 材质和贴图原样带着：删掉的那个面可能是某种材质唯一的使用者，那也没什么 —— 用不到的材质
    // 在 `SplitByMaterial` 那一步自己就不出现了，不必在这儿猜。
    out.materials = description.materials;
    out.textures = description.textures;
    error.clear();
    return true;
}

inline std::vector<MaterialSplit> SplitByMaterial(const MeshDescription& description) {
    std::vector<MeshMaterial> materials = description.materials;
    if (materials.empty()) {
        materials.push_back(MeshMaterial{});
    }

    std::vector<MaterialSplit> splits(materials.size());
    for (std::size_t index = 0; index < materials.size(); ++index) {
        splits[index].material = materials[index];
        splits[index].description.materials.push_back(materials[index]);
        // **贴图每份都跟着走一遍。** 材质里的贴图是个下标，拆完之后还得指得到东西；拷一份索引表
        // 比让每个调用方去对齐两份索引简单。图不多，这份拷贝不是瓶颈。
        splits[index].description.textures = description.textures;
    }

    // 每个材质自己的一份"老点 → 新点"，因为同一种材质里也不是每个点都用得上。
    constexpr std::uint32_t kUnused = 0xFFFF'FFFFu;
    std::vector<std::vector<std::uint32_t>> moved(
        materials.size(), std::vector<std::uint32_t>(description.positions.size(), kUnused));

    for (const Face& face : description.faces) {
        const std::uint32_t slot = face.material < materials.size() ? face.material : 0u;
        Face copy = face;
        copy.material = 0;                 // 拆出来的每一份只有一种材质，所以它总是 0 号
        for (std::uint32_t corner = 0; corner < copy.count; ++corner) {
            const std::uint32_t old = copy.corners[corner];
            if (moved[slot][old] == kUnused) {
                moved[slot][old] =
                    static_cast<std::uint32_t>(splits[slot].description.positions.size());
                splits[slot].description.positions.push_back(description.positions[old]);
            }
            copy.corners[corner] = moved[slot][old];
        }
        splits[slot].description.faces.push_back(copy);
    }

    // 一种材质都没被用到的就不给出去：空的描述烘出来是空 `Mesh`，而渲染器拒收空的。
    std::vector<MaterialSplit> kept;
    for (MaterialSplit& split : splits) {
        if (!split.description.faces.empty()) {
            kept.push_back(std::move(split));
        }
    }
    return kept;
}

}  // namespace zlong::engine
