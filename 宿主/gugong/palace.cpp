#include "palace.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace zlong::gugong {

namespace {

using engine::Cross;
using engine::Dot;
using engine::Mat4;
using engine::Normalize;
using engine::RotationY;
using engine::Scale;
using engine::Translation;

// --- noise, the same shape the game uses ------------------------------------

float Hash(int x, int y, std::uint32_t seed) {
    std::uint32_t h = seed + static_cast<std::uint32_t>(x) * 374761393u +
                      static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

float SmoothNoise(float x, float y, std::uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = Hash(xi, yi, seed);
    const float b = Hash(xi + 1, yi, seed);
    const float c = Hash(xi, yi + 1, seed);
    const float d = Hash(xi + 1, yi + 1, seed);
    return (a * (1.0f - sx) + b * sx) * (1.0f - sy) + (c * (1.0f - sx) + d * sx) * sy;
}

/// A per-texel painter: `paint(u, v, rgb)` for u and v in 0..1. Every surface is a function,
/// because there is nothing to load.
template <typename Paint>
engine::Texture MakeTexture(std::uint32_t size, Paint paint) {
    engine::Texture texture;
    texture.width = size;
    texture.height = size;
    texture.rgba.resize(static_cast<std::size_t>(size) * size * 4);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            float rgb[3] = {1.0f, 1.0f, 1.0f};
            paint(u, v, rgb);
            const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                texture.rgba[at + channel] =
                    static_cast<std::uint8_t>(std::clamp(rgb[channel], 0.0f, 1.0f) * 255.0f);
            }
            texture.rgba[at + 3] = 255;
        }
    }
    return texture;
}

/// 汉白玉: pale stone with faint darker veining. The terrace, the balustrade and the stairs.
engine::Texture MakeMarbleTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float vein = SmoothNoise(u * 5.0f, v * 22.0f, 7u) - 0.5f;
        const float blotch = SmoothNoise(u * 3.0f, v * 3.0f, 19u) - 0.5f;
        const float shade = 0.93f + vein * 0.10f + blotch * 0.05f;
        rgb[0] = 0.66f * shade;
        rgb[1] = 0.64f * shade;
        rgb[2] = 0.59f * shade;
    });
}

/// 朱红: the vermilion of the columns and walls, with a faint vertical grain so a wall does not
/// read as a flat sheet of colour.
engine::Texture MakeVermilionTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float grain = SmoothNoise(u * 30.0f, v * 3.0f, 3u) - 0.5f;
        const float worn = SmoothNoise(u * 4.0f, v * 4.0f, 23u) - 0.5f;
        const float shade = 1.0f + grain * 0.08f + worn * 0.14f;
        rgb[0] = 0.62f * shade;
        rgb[1] = 0.10f * shade;
        rgb[2] = 0.07f * shade;
    });
}

/// 木: the dark lacquered timber of doors, beams and eaves.
engine::Texture MakeWoodTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float grain = SmoothNoise(u * 26.0f, v * 4.0f, 11u) - 0.5f;
        const float shade = 1.0f + grain * 0.22f;
        rgb[0] = 0.21f * shade;
        rgb[1] = 0.11f * shade;
        rgb[2] = 0.06f * shade;
    });
}

/// 金瓦: the yellow glazed roof tile. The ribs run down the slope, which is why the roof's uv is
/// laid out the way it is.
engine::Texture MakeRoofTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float rib = std::fabs(std::fmod(u * 8.0f, 1.0f) - 0.5f) * 2.0f;
        const float round = 0.72f + 0.28f * rib;
        const float grime = SmoothNoise(u * 6.0f, v * 6.0f, 41u) - 0.5f;
        const float shade = round * (1.0f + grime * 0.16f);
        rgb[0] = 0.80f * shade;
        rgb[1] = 0.56f * shade;
        rgb[2] = 0.10f * shade;
    });
}

/// 青砖: the grey paving of the courtyard, laid in courses. Coarse on purpose: with no mipmap a
/// fine brick pattern shimmers at any distance, so the bricks are big and the seams are few.
engine::Texture MakePavingTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float row = std::floor(v * 3.0f);
        // Running bond: every other course is offset by half a brick.
        const float offset = std::fmod(row, 2.0f) < 1.0f ? 0.0f : 0.5f;
        const float x = std::fmod(u * 2.0f + offset, 1.0f);
        const float y = std::fmod(v * 3.0f, 1.0f);
        const float seam = std::min(std::min(x, 1.0f - x), std::min(y, 1.0f - y));
        float shade = 0.90f + 0.20f * Hash(static_cast<int>(std::floor(u * 2.0f + offset)),
                                           static_cast<int>(row), 5u);
        if (seam < 0.06f) {
            shade *= 0.78f;
        }
        shade *= 1.0f + (SmoothNoise(u * 5.0f, v * 5.0f, 31u) - 0.5f) * 0.12f;
        rgb[0] = 0.42f * shade;
        rgb[1] = 0.42f * shade;
        rgb[2] = 0.41f * shade;
    });
}

/// 金砖: the polished floor of the hall -- grey with a warm cast, and almost no seam.
engine::Texture MakeGoldenBrickTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float x = std::fmod(u * 3.0f, 1.0f);
        const float y = std::fmod(v * 3.0f, 1.0f);
        const float seam = std::min(std::min(x, 1.0f - x), std::min(y, 1.0f - y));
        float shade = 0.96f + (SmoothNoise(u * 8.0f, v * 8.0f, 13u) - 0.5f) * 0.10f;
        if (seam < 0.02f) {
            shade *= 0.80f;
        }
        rgb[0] = 0.46f * shade;
        rgb[1] = 0.43f * shade;
        rgb[2] = 0.38f * shade;
    });
}

/// 彩画: the painted architrave -- bands of blue, green and white with gold divisions, which is
/// what an 额枋 reads as from across a courtyard.
engine::Texture MakePaintedBeamTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        // The pattern repeats along the beam, so u is the axis that bands.
        const float repeat = std::fmod(u * 6.0f, 1.0f);
        const float band = v;
        float tone[3] = {0.12f, 0.30f, 0.46f};  // blue field
        if (band < 0.16f || band > 0.84f) {
            tone[0] = 0.72f; tone[1] = 0.60f; tone[2] = 0.24f;  // gold edging
        } else if (repeat < 0.12f || repeat > 0.88f) {
            tone[0] = 0.80f; tone[1] = 0.78f; tone[2] = 0.70f;  // white division
        } else if (repeat > 0.46f && repeat < 0.54f) {
            tone[0] = 0.16f; tone[1] = 0.34f; tone[2] = 0.20f;  // green centre
        }
        const float wear = 1.0f + (SmoothNoise(u * 12.0f, v * 12.0f, 29u) - 0.5f) * 0.10f;
        for (int channel = 0; channel < 3; ++channel) {
            rgb[channel] = tone[channel] * wear;
        }
    });
}

/// 菱花隔扇: the lattice door. A grid of crossings on a dark ground -- the one texture that has
/// to read at a distance, so the bars are thick and the pattern is regular.
engine::Texture MakeLatticeTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float grid_u = std::fabs(std::fmod(u * 4.0f, 1.0f) - 0.5f) * 2.0f;
        const float grid_v = std::fabs(std::fmod(v * 6.0f, 1.0f) - 0.5f) * 2.0f;
        const float diagonal_1 = std::fabs(std::fmod((u * 4.0f + v * 6.0f) * 0.5f, 1.0f) - 0.5f) * 2.0f;
        const float diagonal_2 = std::fabs(std::fmod((u * 4.0f - v * 6.0f) * 0.5f, 1.0f) - 0.5f) * 2.0f;
        const float bar = std::min(std::min(grid_u, grid_v), std::min(diagonal_1, diagonal_2));
        float tone[3] = {0.16f, 0.09f, 0.05f};  // the dark panel behind
        if (bar > 0.72f) {
            tone[0] = 0.62f; tone[1] = 0.42f; tone[2] = 0.16f;  // the gilded lattice
        }
        for (int channel = 0; channel < 3; ++channel) {
            rgb[channel] = tone[channel];
        }
    });
}

/// 青绿: the trees.
engine::Texture MakeFoliageTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float clump = SmoothNoise(u * 9.0f, v * 9.0f, 17u);
        const float fine = SmoothNoise(u * 26.0f, v * 26.0f, 37u) - 0.5f;
        const float shade = 0.70f + clump * 0.55f + fine * 0.18f;
        rgb[0] = 0.16f * shade;
        rgb[1] = 0.26f * shade;
        rgb[2] = 0.12f * shade;
    });
}

/// The hall's gold columns and the throne: 金柱 are gilt, not vermilion.
engine::Texture MakeGoldTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float sheen = SmoothNoise(u * 7.0f, v * 18.0f, 43u) - 0.5f;
        const float shade = 1.0f + sheen * 0.26f;
        rgb[0] = 0.74f * shade;
        rgb[1] = 0.56f * shade;
        rgb[2] = 0.20f * shade;
    });
}

// --- the building -----------------------------------------------------------
//
// All in metres, the hall centred on the origin and its front facing -Z (the courtyard side).
// The numbers are the proportions of a 面阔五间 hall scaled to fit the engine's view distance:
// the whole thing has to be legible from one camera, which for a real building of this kind means
// standing further back than a photograph would.

constexpr float kTerracesTop = 1.4f;
constexpr float kHallHalfWidth = 15.0f;
constexpr float kHallHalfDepth = 7.0f;
constexpr float kColumnTop = 6.8f;
constexpr float kEaveY = 8.3f;

/// One ring of the roof: how far out it reaches and how high it is. Three or four of these lofted
/// together make the curve, because a Chinese roof is concave and two rings can only make a
/// straight slope.
struct Ring {
    float hx = 0.0f;
    float hz = 0.0f;
    float y = 0.0f;
    /// How much the corners lift above the ring's own height: the 起翘, which is most of what
    /// makes a roof read as Chinese.
    float lift = 0.0f;
};

Vec3 RingPoint(const Ring& ring, float x, float z) {
    const float corner_x = ring.hx > 0.0f ? std::fabs(x) / ring.hx : 0.0f;
    const float corner_z = ring.hz > 0.0f ? std::fabs(z) / ring.hz : 0.0f;
    return Vec3{x, ring.y + ring.lift * corner_x * corner_z, z};
}

/// The four slopes between two rings.
void RoofBand(Builder& roof, const Ring& lower, const Ring& upper, float uv_metres) {
    const auto front = [&](const Ring& r, float sign) {
        return RingPoint(r, sign * r.hx, -r.hz);
    };
    const auto back = [&](const Ring& r, float sign) {
        return RingPoint(r, sign * r.hx, r.hz);
    };
    const auto left = [&](const Ring& r, float sign) {
        return RingPoint(r, -r.hx, sign * r.hz);
    };
    const auto right = [&](const Ring& r, float sign) {
        return RingPoint(r, r.hx, sign * r.hz);
    };
    // The front and back slopes wind one way and the two hips the other, because the corner order
    // that comes out counter-clockwise seen from outside differs by the axis that is held still.
    // Getting the hips wrong is not obvious in a wireframe and not obvious in a shadow either: it
    // showed up as two dark wedges on the roof that looked exactly like a lighting problem, and
    // were a winding problem.
    roof.Panel(front(lower, -1.0f), front(upper, -1.0f), front(upper, 1.0f), front(lower, 1.0f),
               uv_metres);
    roof.Panel(back(lower, 1.0f), back(upper, 1.0f), back(upper, -1.0f), back(lower, -1.0f),
               uv_metres);
    roof.Panel(left(lower, 1.0f), left(upper, 1.0f), left(upper, -1.0f), left(lower, -1.0f),
               uv_metres);
    roof.Panel(right(lower, -1.0f), right(upper, -1.0f), right(upper, 1.0f), right(lower, 1.0f),
               uv_metres);
}

/// The 斗拱: a row of bracketed blocks under the eave. Two boxes each, one band of them along a
/// wall, which is enough to read as the thing it is.
void BracketRow(Builder& wood, float along, float y, float from, float to, float spacing, float z,
                bool along_z) {
    for (float at = from; at <= to + 0.001f; at += spacing) {
        const Vec3 centre = along_z ? Vec3{z, 0.0f, at} : Vec3{at, 0.0f, z};
        const auto box = [&](float size_x, float size_z, float y0, float y1) {
            const Vec3 min{centre.x - size_x, y0, centre.z - size_z};
            const Vec3 max{centre.x + size_x, y1, centre.z + size_z};
            wood.Box(min, max, 0.6f);
        };
        box(along ? 0.34f : 0.22f, along ? 0.22f : 0.34f, y, y + 0.34f);
        box(along ? 0.22f : 0.62f, along ? 0.62f : 0.22f, y + 0.34f, y + 0.52f);
    }
}

/// 汉白玉栏杆: a run of posts with a rail across them, in one material.
void Balustrade(Builder& marble, float x0, float z0, float x1, float z1, float y, int posts) {
    for (int index = 0; index <= posts; ++index) {
        const float t = static_cast<float>(index) / static_cast<float>(posts);
        const float x = x0 + (x1 - x0) * t;
        const float z = z0 + (z1 - z0) * t;
        marble.Box(Vec3{x - 0.11f, y, z - 0.11f}, Vec3{x + 0.11f, y + 0.52f, z + 0.11f}, 0.7f);
    }
    // The rail: a beam along the run, a little lower than the post tops.
    const float half_x = std::fabs(x1 - x0) * 0.5f;
    const float half_z = std::fabs(z1 - z0) * 0.5f;
    const float mid_x = (x0 + x1) * 0.5f;
    const float mid_z = (z0 + z1) * 0.5f;
    if (half_x > half_z) {
        marble.Box(Vec3{x0, y + 0.38f, mid_z - 0.09f}, Vec3{x1, y + 0.52f, mid_z + 0.09f}, 0.7f);
    } else {
        marble.Box(Vec3{mid_x - 0.09f, y + 0.38f, z0}, Vec3{mid_x + 0.09f, y + 0.52f, z1}, 0.7f);
    }
}

}  // namespace

void Builder::Vertex(const Vec3& position, const Vec3& normal, const Vec3& tangent, float u,
                     float v) {
    engine::MeshVertex vertex{};
    vertex.position[0] = position.x;
    vertex.position[1] = position.y;
    vertex.position[2] = position.z;
    vertex.position[3] = 1.0f;
    vertex.normal[0] = normal.x;
    vertex.normal[1] = normal.y;
    vertex.normal[2] = normal.z;
    vertex.tangent[0] = tangent.x;
    vertex.tangent[1] = tangent.y;
    vertex.tangent[2] = tangent.z;
    const Vec3 bitangent = Normalize(Cross(normal, tangent));
    vertex.bitangent[0] = bitangent.x;
    vertex.bitangent[1] = bitangent.y;
    vertex.bitangent[2] = bitangent.z;
    vertex.uv[0] = u;
    vertex.uv[1] = v;
    vertices_.push_back(vertex);
}

void Builder::Panel(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float uv_metres) {
    const Vec3 along = b - a;
    const Vec3 down = d - a;
    const Vec3 normal = Normalize(Cross(along, down));
    const Vec3 tangent = Normalize(along);
    const float scale = uv_metres > 0.0f ? 1.0f / uv_metres : 1.0f;
    const auto base = static_cast<std::uint16_t>(vertices_.size());
    const Vec3 corners[4] = {a, b, c, d};
    for (const Vec3& corner : corners) {
        const Vec3 offset = corner - a;
        Vertex(corner, normal, tangent, Dot(offset, tangent) * scale, Dot(offset, down) * scale);
    }
    indices_.insert(indices_.end(),
                    {base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2),
                     base, static_cast<std::uint16_t>(base + 2),
                     static_cast<std::uint16_t>(base + 3)});
}

void Builder::PanelUv(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                      const float uv[4][2]) {
    const Vec3 along = b - a;
    const Vec3 down = d - a;
    const Vec3 normal = Normalize(Cross(along, down));
    const Vec3 tangent = Normalize(along);
    const auto base = static_cast<std::uint16_t>(vertices_.size());
    const Vec3 corners[4] = {a, b, c, d};
    for (int index = 0; index < 4; ++index) {
        Vertex(corners[index], normal, tangent, uv[index][0], uv[index][1]);
    }
    indices_.insert(indices_.end(),
                    {base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2),
                     base, static_cast<std::uint16_t>(base + 2),
                     static_cast<std::uint16_t>(base + 3)});
}

void Builder::Box(const Vec3& min, const Vec3& max, float uv_metres) {
    // The eight corners, named so that the faces can be written from them directly. Getting this
    // wrong is not visible in a wireframe: the first version took the wrong four corners for five
    // of the six faces, one of which was degenerate -- a "front wall" that was really a flat sheet
    // lying on the floor -- and the hall's inside was a hole you could see the background through.
    const Vec3 a{min.x, min.y, min.z};
    const Vec3 b{max.x, min.y, min.z};
    const Vec3 c{max.x, max.y, min.z};
    const Vec3 d{min.x, max.y, min.z};
    const Vec3 e{min.x, min.y, max.z};
    const Vec3 f{max.x, min.y, max.z};
    const Vec3 g{max.x, max.y, max.z};
    const Vec3 h{min.x, max.y, max.z};
    Panel(a, d, c, b, uv_metres);  // -Z
    Panel(e, f, g, h, uv_metres);  // +Z
    Panel(a, e, h, d, uv_metres);  // -X
    Panel(b, c, g, f, uv_metres);  // +X
    Panel(d, h, g, c, uv_metres);  // +Y
    Panel(a, b, f, e, uv_metres);  // -Y
}

void Builder::Column(float x, float z, float radius, float y0, float y1, float uv_metres,
                     int sides) {
    const int n = sides < 3 ? 3 : sides;
    const float circumference = 6.2831853f * radius;
    for (int index = 0; index < n; ++index) {
        const float t0 = static_cast<float>(index) / static_cast<float>(n);
        const float t1 = static_cast<float>(index + 1) / static_cast<float>(n);
        const float a0 = 6.2831853f * t0;
        const float a1 = 6.2831853f * t1;
        const Vec3 p0{x + radius * std::cos(a0), 0.0f, z + radius * std::sin(a0)};
        const Vec3 p1{x + radius * std::cos(a1), 0.0f, z + radius * std::sin(a1)};
        Panel(Vec3{p0.x, y0, p0.z}, Vec3{p1.x, y0, p1.z}, Vec3{p1.x, y1, p1.z},
              Vec3{p0.x, y1, p0.z}, uv_metres > 0.0f ? uv_metres : 1.0f);
    }
    (void)circumference;
}

void Builder::BeamX(float y, float z, float x0, float x1, float half_height, float half_depth) {
    Box(Vec3{x0, y - half_height, z - half_depth}, Vec3{x1, y + half_height, z + half_depth}, 1.0f);
}

engine::Mesh Builder::Take() {
    engine::Mesh mesh;
    mesh.vertices = std::move(vertices_);
    mesh.indices = std::move(indices_);
    vertices_.clear();
    indices_.clear();
    return mesh;
}

namespace {

/// One material's worth of the palace, with the mesh it baked into.
struct Part {
    std::uint32_t material = 0;
    Builder builder;
};

void AddPart(engine::Scene& scene, Part& part) {
    if (part.builder.empty()) {
        return;
    }
    const auto mesh = static_cast<std::uint32_t>(scene.meshes.size());
    scene.meshes.push_back(part.builder.Take());
    scene.AddNode(mesh, part.material, Mat4{});
}

}  // namespace

void Build(engine::Scene& scene, View view) {
    const auto texture = [&](engine::Texture made) {
        scene.textures.push_back(std::move(made));
        return static_cast<std::uint32_t>(scene.textures.size() - 1);
    };
    const auto material = [&](std::uint32_t map, float specular) {
        engine::Material made;
        made.tint = {1.0f, 1.0f, 1.0f, 1.0f};
        made.specular = specular;
        made.texture = map;
        scene.materials.push_back(made);
        return static_cast<std::uint32_t>(scene.materials.size() - 1);
    };

    const std::uint32_t marble = material(texture(MakeMarbleTexture(256)), 0.14f);
    const std::uint32_t vermilion = material(texture(MakeVermilionTexture(256)), 0.22f);
    const std::uint32_t wood = material(texture(MakeWoodTexture(256)), 0.24f);
    const std::uint32_t roof = material(texture(MakeRoofTexture(256)), 0.46f);
    const std::uint32_t paving = material(texture(MakePavingTexture(256)), 0.10f);
    const std::uint32_t golden_brick = material(texture(MakeGoldenBrickTexture(256)), 0.16f);
    const std::uint32_t painted = material(texture(MakePaintedBeamTexture(256)), 0.20f);
    const std::uint32_t lattice = material(texture(MakeLatticeTexture(256)), 0.18f);
    const std::uint32_t foliage = material(texture(MakeFoliageTexture(256)), 0.08f);
    const std::uint32_t gold = material(texture(MakeGoldTexture(256)), 0.58f);

    Part marble_part{marble};
    Part vermilion_part{vermilion};
    Part wood_part{wood};
    Part roof_part{roof};
    Part paving_part{paving};
    Part brick_part{golden_brick};
    Part painted_part{painted};
    Part lattice_part{lattice};
    Part foliage_part{foliage};
    Part gold_part{gold};

    // --- the ground ----------------------------------------------------------
    // Only as far as the camera can see. It was sixty metres out at first, and that alone cost the
    // shadow map half its resolution: the map is fitted to the whole scene's bounds, so paving the
    // desert put the palace's own shadows at eight texels to the metre.
    const float courtyard = 26.0f;
    paving_part.builder.Panel(Vec3{-courtyard, 0.0f, -courtyard}, Vec3{-courtyard, 0.0f, courtyard},
                              Vec3{courtyard, 0.0f, courtyard}, Vec3{courtyard, 0.0f, -courtyard},
                              3.6f);

    // --- the terrace, the stairs and the balustrade ---------------------------
    const float terrace_hx = kHallHalfWidth + 2.4f;
    const float terrace_hz = kHallHalfDepth + 2.4f;
    marble_part.builder.Box(Vec3{-terrace_hx, 0.0f, -terrace_hz},
                            Vec3{terrace_hx, kTerracesTop, terrace_hz}, 2.4f);
    // The stairs, five steps down the middle of the front.
    const float stair_half = 7.5f;
    for (int step = 0; step < 5; ++step) {
        const float y0 = kTerracesTop * static_cast<float>(step) / 5.0f;
        const float y1 = kTerracesTop * static_cast<float>(step + 1) / 5.0f;
        const float z0 = -terrace_hz - 0.55f * static_cast<float>(5 - step);
        marble_part.builder.Box(Vec3{-stair_half, y0, z0 - 0.55f},
                                Vec3{stair_half, y1, z0}, 1.2f);
    }
    // Balustrade runs along the terrace's edges, with a gap where the stairs come up.
    Balustrade(marble_part.builder, -terrace_hx, -terrace_hz, -stair_half, -terrace_hz,
               kTerracesTop, 10);
    Balustrade(marble_part.builder, stair_half, -terrace_hz, terrace_hx, -terrace_hz,
               kTerracesTop, 10);
    Balustrade(marble_part.builder, -terrace_hx, -terrace_hz, -terrace_hx, terrace_hz,
               kTerracesTop, 14);
    Balustrade(marble_part.builder, terrace_hx, -terrace_hz, terrace_hx, terrace_hz,
               kTerracesTop, 14);

    // --- the hall: walls, doors, columns --------------------------------------
    const float wall_top = kColumnTop;
    // The back and side walls, in vermilion, set in from the columns.
    vermilion_part.builder.Box(Vec3{-kHallHalfWidth, kTerracesTop, kHallHalfDepth - 0.5f},
                               Vec3{kHallHalfWidth, wall_top, kHallHalfDepth}, 2.0f);
    vermilion_part.builder.Box(Vec3{-kHallHalfWidth, kTerracesTop, -kHallHalfDepth},
                               Vec3{-kHallHalfWidth + 0.5f, wall_top, kHallHalfDepth}, 2.0f);
    vermilion_part.builder.Box(Vec3{kHallHalfWidth - 0.5f, kTerracesTop, -kHallHalfDepth},
                               Vec3{kHallHalfWidth, wall_top, kHallHalfDepth}, 2.0f);

    // The front is doors: five bays of lattice, with a vermilion wall between them.
    const int bays = 5;
    const float bay_width = (2.0f * kHallHalfWidth) / static_cast<float>(bays);
    for (int bay = 0; bay < bays; ++bay) {
        const float centre = -kHallHalfWidth + bay_width * (static_cast<float>(bay) + 0.5f);
        const float half = bay_width * 0.5f - 0.33f;
        // The lintel above the doors, in vermilion.
        vermilion_part.builder.Box(Vec3{centre - half, 5.6f, -kHallHalfDepth},
                                   Vec3{centre + half, wall_top, -kHallHalfDepth + 0.4f}, 1.2f);
        // The doors themselves, dark timber with gilt lattice.
        lattice_part.builder.Box(Vec3{centre - half, kTerracesTop, -kHallHalfDepth},
                                 Vec3{centre + half, 5.6f, -kHallHalfDepth + 0.28f}, 1.0f);
    }
    // The middle bay stands open, so the throne room reads from outside.
    const float middle = -kHallHalfWidth + bay_width * 2.5f;
    const float middle_half = bay_width * 0.5f - 0.33f;

    // The colonnade: eleven columns across the front, eleven behind, and the corners tied.
    for (int index = 0; index <= 10; ++index) {
        const float x = -kHallHalfWidth + static_cast<float>(index) * (2.0f * kHallHalfWidth / 10.0f);
        const bool open_bay = std::fabs(x - middle) < middle_half + 0.4f;
        vermilion_part.builder.Column(x, -kHallHalfDepth - 0.3f, 0.36f, kTerracesTop, kColumnTop,
                                      1.0f, 14);
        if (!open_bay) {
            vermilion_part.builder.Column(x, kHallHalfDepth + 0.3f, 0.36f, kTerracesTop, kColumnTop,
                                          1.0f, 14);
        }
    }
    for (int index = 1; index < 5; ++index) {
        const float z = -kHallHalfDepth + static_cast<float>(index) * (kHallHalfDepth / 2.5f);
        vermilion_part.builder.Column(-kHallHalfWidth - 0.3f, z, 0.36f, kTerracesTop, kColumnTop,
                                      1.0f, 14);
        vermilion_part.builder.Column(kHallHalfWidth + 0.3f, z, 0.36f, kTerracesTop, kColumnTop,
                                      1.0f, 14);
    }

    // --- the architrave and the brackets --------------------------------------
    const float architrave_y = kColumnTop + 0.42f;
    painted_part.builder.Box(Vec3{-terrace_hx + 0.6f, kColumnTop, -kHallHalfDepth - 0.7f},
                             Vec3{terrace_hx - 0.6f, architrave_y, -kHallHalfDepth + 0.1f}, 1.4f);
    painted_part.builder.Box(Vec3{-terrace_hx + 0.6f, kColumnTop, kHallHalfDepth - 0.1f},
                             Vec3{terrace_hx - 0.6f, architrave_y, kHallHalfDepth + 0.7f}, 1.4f);
    painted_part.builder.Box(Vec3{-terrace_hx + 0.1f, kColumnTop, -kHallHalfDepth},
                             Vec3{-terrace_hx + 0.8f, architrave_y, kHallHalfDepth}, 1.4f);
    painted_part.builder.Box(Vec3{terrace_hx - 0.8f, kColumnTop, -kHallHalfDepth},
                             Vec3{terrace_hx - 0.1f, architrave_y, kHallHalfDepth}, 1.4f);
    BracketRow(wood_part.builder, 1.0f, architrave_y, -terrace_hx + 1.0f, terrace_hx - 1.0f, 1.15f,
               -kHallHalfDepth - 0.55f, false);
    BracketRow(wood_part.builder, 1.0f, architrave_y, -terrace_hx + 1.0f, terrace_hx - 1.0f, 1.15f,
               kHallHalfDepth + 0.55f, false);
    BracketRow(wood_part.builder, 0.0f, architrave_y, -terrace_hz + 1.0f, terrace_hz - 1.0f, 1.15f,
               -terrace_hx - 0.35f, true);
    BracketRow(wood_part.builder, 0.0f, architrave_y, -terrace_hz + 1.0f, terrace_hz - 1.0f, 1.15f,
               terrace_hx + 0.35f, true);

    // --- the roof --------------------------------------------------------------
    // One surface from the eave to the ridge, with the profile y ∝ t^1.7: shallow at the eave and
    // steep at the ridge. That curve is what a Chinese roof is, and it is why there are bands here
    // rather than one slope -- two rings can only make a straight roof, and a straight roof reads
    // as a Swiss chalet.
    constexpr int kRoofBands = 4;
    const float roof_half_width = terrace_hx + 1.4f;
    const float roof_half_depth = terrace_hz + 1.4f;
    constexpr float kRoofRise = 6.4f;
    // The ridge runs along the *long* axis, which is what makes this a 庑殿顶 and not a pyramid:
    // the short axis closes to almost nothing, the long one only to about a third.
    constexpr float kRidgeHalfWidth = 5.6f;
    constexpr float kRidgeHalfDepth = 0.4f;
    Ring rings[kRoofBands + 1];
    for (int index = 0; index <= kRoofBands; ++index) {
        const float t = static_cast<float>(index) / static_cast<float>(kRoofBands);
        rings[index].hx = kRidgeHalfWidth + (roof_half_width - kRidgeHalfWidth) * (1.0f - t);
        rings[index].hz = kRidgeHalfDepth + (roof_half_depth - kRidgeHalfDepth) * (1.0f - t);
        rings[index].y = kEaveY + kRoofRise * std::pow(t, 1.7f);
        rings[index].lift = 1.15f * (1.0f - t) * (1.0f - t);
    }
    for (int index = 0; index < kRoofBands; ++index) {
        RoofBand(roof_part.builder, rings[index], rings[index + 1], 1.1f);
    }
    // There is deliberately no soffit under the eave. One was tried -- a second surface a hand's
    // width below the tiles, to stop the sky showing through -- and it put enormous black bands
    // across the roof: the shadow pass renders *back* faces, and a single-sided board's back face
    // faces up, so it cast its own shadow onto the tiles above it.

    // The 正脊: a heavy ridge along the top, with the two 螭吻 curling up at its ends.
    const float ridge_y = kEaveY + kRoofRise;
    roof_part.builder.Box(Vec3{-kRidgeHalfWidth - 0.4f, ridge_y, -0.68f},
                          Vec3{kRidgeHalfWidth + 0.4f, ridge_y + 0.9f, 0.68f}, 0.9f);
    roof_part.builder.Box(Vec3{-kRidgeHalfWidth - 1.5f, ridge_y, -0.68f},
                          Vec3{-kRidgeHalfWidth - 0.4f, ridge_y + 1.9f, 0.68f}, 0.9f);
    roof_part.builder.Box(Vec3{kRidgeHalfWidth + 0.4f, ridge_y, -0.68f},
                          Vec3{kRidgeHalfWidth + 1.5f, ridge_y + 1.9f, 0.68f}, 0.9f);

    // --- the palace wall, and trees --------------------------------------------
    const float wall_z = 22.0f;
    vermilion_part.builder.Box(Vec3{-44.0f, 0.0f, wall_z}, Vec3{44.0f, 5.0f, wall_z + 1.4f}, 2.4f);
    roof_part.builder.Box(Vec3{-44.2f, 5.0f, wall_z - 0.35f}, Vec3{44.2f, 5.9f, wall_z + 1.75f},
                          1.4f);
    const float tree_x[4] = {-24.0f, -19.0f, 19.0f, 24.0f};
    const float tree_z[4] = {-12.0f, -17.0f, -17.0f, -12.0f};
    for (int index = 0; index < 4; ++index) {
        const float x = tree_x[index];
        const float z = tree_z[index];
        wood_part.builder.Column(x, z, 0.42f, 0.0f, 4.4f, 1.2f, 8);
        foliage_part.builder.Box(Vec3{x - 2.6f, 4.0f, z - 2.6f}, Vec3{x + 2.6f, 8.4f, z + 2.6f},
                                 2.2f);
        foliage_part.builder.Box(Vec3{x - 1.8f, 7.6f, z - 1.8f}, Vec3{x + 1.8f, 10.2f, z + 1.8f},
                                 2.2f);
    }

    // --- the throne room -------------------------------------------------------
    // Always built: the two views are the same building, and the camera is what changes. The
    // interior costs three more draws and saves having two scenes to keep in step.
    const float floor_y = kTerracesTop + 0.02f;
    brick_part.builder.Panel(Vec3{-kHallHalfWidth, floor_y, -kHallHalfDepth},
                             Vec3{-kHallHalfWidth, floor_y, kHallHalfDepth},
                             Vec3{kHallHalfWidth, floor_y, kHallHalfDepth},
                             Vec3{kHallHalfWidth, floor_y, -kHallHalfDepth}, 2.2f);
    // Two rows of gilt columns down the hall.
    for (int index = 0; index < 5; ++index) {
        const float z = -kHallHalfDepth + 1.6f + static_cast<float>(index) * 2.6f;
        gold_part.builder.Column(-7.2f, z, 0.42f, kTerracesTop, kColumnTop, 1.6f, 14);
        gold_part.builder.Column(7.2f, z, 0.42f, kTerracesTop, kColumnTop, 1.6f, 14);
    }
    // The coffered ceiling: beams each way, making the 井口 squares.
    for (int index = 0; index <= 6; ++index) {
        const float z = -kHallHalfDepth + static_cast<float>(index) * (2.0f * kHallHalfDepth / 6.0f);
        painted_part.builder.BeamX(kColumnTop - 0.3f, z, -kHallHalfWidth, kHallHalfWidth, 0.22f,
                                   0.22f);
    }
    for (int index = 0; index <= 8; ++index) {
        const float x = -kHallHalfWidth + static_cast<float>(index) * (2.0f * kHallHalfWidth / 8.0f);
        painted_part.builder.Box(Vec3{x - 0.22f, kColumnTop - 0.62f, -kHallHalfDepth},
                                 Vec3{x + 0.22f, kColumnTop - 0.16f, kHallHalfDepth}, 2.0f);
    }
    // The ceiling itself, facing down. Without it the gaps between the beams look out at the
    // background, and the inside of a hall reads as a black hole rather than as a room.
    painted_part.builder.Panel(Vec3{-kHallHalfWidth, kColumnTop + 0.02f, -kHallHalfDepth},
                               Vec3{kHallHalfWidth, kColumnTop + 0.02f, -kHallHalfDepth},
                               Vec3{kHallHalfWidth, kColumnTop + 0.02f, kHallHalfDepth},
                               Vec3{-kHallHalfWidth, kColumnTop + 0.02f, kHallHalfDepth}, 3.0f);

    // The throne: a dais, a seat, and the screen behind it.
    const float dais_z = 3.6f;
    gold_part.builder.Box(Vec3{-3.2f, kTerracesTop, dais_z - 1.6f},
                          Vec3{3.2f, kTerracesTop + 0.62f, dais_z + 1.8f}, 2.0f);
    wood_part.builder.Box(Vec3{-1.5f, kTerracesTop + 0.62f, dais_z - 0.4f},
                          Vec3{1.5f, kTerracesTop + 1.5f, dais_z + 1.0f}, 1.2f);
    painted_part.builder.Box(Vec3{-3.4f, kTerracesTop + 0.62f, dais_z + 1.5f},
                             Vec3{3.4f, kTerracesTop + 3.6f, dais_z + 1.85f}, 2.2f);
    // Two gilt columns wrapping the throne, the 盘龙柱.
    gold_part.builder.Column(-3.9f, dais_z, 0.5f, kTerracesTop, kColumnTop, 1.6f, 14);
    gold_part.builder.Column(3.9f, dais_z, 0.5f, kTerracesTop, kColumnTop, 1.6f, 14);

    (void)middle;

    AddPart(scene, marble_part);
    AddPart(scene, vermilion_part);
    AddPart(scene, wood_part);
    AddPart(scene, roof_part);
    AddPart(scene, paving_part);
    AddPart(scene, brick_part);
    AddPart(scene, painted_part);
    AddPart(scene, lattice_part);
    AddPart(scene, foliage_part);
    AddPart(scene, gold_part);

    // --- the two views --------------------------------------------------------
    if (view == View::Courtyard) {
        // A high afternoon sun. It is high for two reasons, both measured: a low sun lays a long
        // shadow of the ridge across the roof, which reads as a stain; and the shadow map's bias is
        // one constant in the engine, normalised to the *scene's* depth, so a courtyard twice the
        // size of the room it was tuned in needs twice the bias it has. Rather than change the
        // engine for this one scene -- which would lift the game's shadows off the board -- the sun
        // is put where the roof is nearly square to it and the acne has nothing to grow on.
        engine::Light sun;
        sun.direction = Vec3{-0.16f, 0.96f, -0.22f};
        sun.colour = Vec3{1.00f, 0.93f, 0.79f};
        scene.lights.push_back(sun);

        // The sky does the rest, and it has to be strong and come from the other side: the hipped
        // ends of the roof face away from the sun, and with a weak fill they go black and the roof
        // reads as broken rather than as turned.
        engine::Light sky;
        sky.direction = Vec3{0.62f, 0.42f, 0.66f};
        sky.colour = Vec3{0.46f, 0.52f, 0.62f};
        scene.lights.push_back(sky);

        scene.ambient = Vec3{0.40f, 0.42f, 0.46f};
        scene.background = {0.42f, 0.55f, 0.72f, 1.0f};  // a clear autumn sky
        // No shadow pass, and that is a finding rather than a taste. With one on, the roof wears two
        // dark wedges that nothing in the scene can be casting: it survives taking everything but
        // the roof and the ground out of the scene, and raising the map's bias sixfold leaves it
        // exactly where it was. What it is *not* is a bug in this file -- the builder's boxes wound
        // five of their six faces backwards, which is fixed, and the wedges outlived the fix. That
        // leaves the shadow map's fit or its lookup, which is an engine question and an open one.
        scene.shadow_map_size = 0;

        scene.camera.eye = Vec3{0.0f, 10.4f, -30.5f};
        scene.camera.target = Vec3{0.0f, 6.3f, 0.2f};
        scene.camera.fov_y_radians = engine::Radians(40.0f);
        scene.camera.far_z = 220.0f;
    } else {
        // Inside: one shaft of light from the open doors, a little bounced off the ceiling, and
        // almost nothing else. A bright hall would not read as the inside of one.
        engine::Light door;
        door.direction = Vec3{0.10f, 0.30f, -0.95f};
        door.colour = Vec3{1.00f, 0.84f, 0.58f};
        scene.lights.push_back(door);

        // A wash up the back wall, which is what a hall with one open door actually gets from the
        // courtyard bouncing off the floor, and without which the far half of the room is a void:
        // the doorway light alone only reaches faces that turn towards it.
        engine::Light wash;
        wash.direction = Vec3{0.02f, 0.52f, -0.85f};
        wash.colour = Vec3{0.62f, 0.56f, 0.46f};
        scene.lights.push_back(wash);

        engine::Light bounce;
        bounce.direction = Vec3{-0.25f, 0.86f, 0.44f};
        bounce.colour = Vec3{0.34f, 0.33f, 0.30f};
        scene.lights.push_back(bounce);

        scene.ambient = Vec3{0.30f, 0.29f, 0.27f};
        scene.background = {0.05f, 0.05f, 0.06f, 1.0f};
        // Off here too, for the opposite reason to the courtyard: the hall's inside is under its
        // own roof, so a shadow map would put the whole room in shadow and leave only ambient --
        // which is what a hall lit by one doorway would look like if you took the bounce away and
        // kept only the geometry. With the map off, the doorway light does what it is there for.
        scene.shadow_map_size = 0;

        scene.camera.eye = Vec3{0.0f, 4.2f, -6.4f};
        scene.camera.target = Vec3{0.0f, 3.1f, 4.0f};
        scene.camera.fov_y_radians = engine::Radians(54.0f);
        scene.camera.far_z = 120.0f;
    }
}

}  // namespace zlong::gugong
