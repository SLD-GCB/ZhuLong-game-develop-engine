#include "zlong/gpu/software/rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include "zlong/gpu/shader/interp.h"
#include "zlong/gpu/software/target.h"
#include "zlong/gpu/surface/texel.h"

namespace zlong::gpu::software {

namespace {

using Vec4 = std::array<float, 4>;
using render::BlendFactor;
using render::BlendOp;
using render::CompareOp;
using render::DrawDesc;
using render::RenderResult;
using render::RenderStatus;

struct ShadedVertex {
    Vec4 clip{};
    /// Vertex shader outputs after the position (index 0).
    std::vector<Vec4> varyings;
};

struct ScreenVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float inv_w = 1.0f;
};

// ---------------------------------------------------------- near-plane clip --

/// How far in front of the near plane a clipped vertex is left. It has to be
/// strictly positive: a vertex exactly ON the plane has w = 0, and `to_screen`
/// has nothing to divide by.
constexpr float kNearPlaneEpsilon = 1e-5f;

/// Clip-space distance to the near plane -- what a standard projection puts at
/// NDC z = -1. Negative means behind it, and w <= 0 falls out of the same test,
/// which is the condition the old code checked by dropping the whole triangle.
float NearDistance(const Vec4& clip) {
    return clip[2] + clip[3] - kNearPlaneEpsilon;
}

/// Linear interpolation in CLIP space. This is what keeps the varyings
/// perspective-correct: the divide by w happens afterwards, on each new vertex's
/// own w, so the perspective applies to the clipped geometry rather than to the
/// original corners.
ShadedVertex LerpVertex(const ShadedVertex& from, const ShadedVertex& to, float t) {
    ShadedVertex out;
    for (int lane = 0; lane < 4; ++lane) {
        out.clip[lane] = from.clip[lane] + (to.clip[lane] - from.clip[lane]) * t;
    }
    out.varyings.resize(from.varyings.size());
    for (std::size_t varying = 0; varying < from.varyings.size(); ++varying) {
        for (int lane = 0; lane < 4; ++lane) {
            out.varyings[varying][lane] = from.varyings[varying][lane] +
                                          (to.varyings[varying][lane] -
                                           from.varyings[varying][lane]) *
                                              t;
        }
    }
    return out;
}

/// Sutherland-Hodgman against the single near plane. Three or four vertices
/// come out; zero means the triangle was entirely behind.
///
/// Only the near plane is cut here. The side planes are handled by clamping the
/// bounding box to the target, and the far plane needs no help: geometry beyond
/// it stays bounded, so it is drawn rather than dropped.
struct ClippedPolygon {
    ShadedVertex vertices[4];
    std::size_t count = 0;
};

ClippedPolygon ClipToNearPlane(const ShadedVertex& a, const ShadedVertex& b,
                               const ShadedVertex& c) {
    const ShadedVertex* corners[3] = {&a, &b, &c};
    ClippedPolygon out;
    for (int corner = 0; corner < 3; ++corner) {
        const ShadedVertex& current = *corners[corner];
        const ShadedVertex& next = *corners[(corner + 1) % 3];
        const float current_distance = NearDistance(current.clip);
        const float next_distance = NearDistance(next.clip);

        if (current_distance >= 0.0f) {
            out.vertices[out.count++] = current;
        }
        // A crossing keeps the winding order, so what comes out is still a
        // polygon and not a self-intersecting one.
        if ((current_distance >= 0.0f) != (next_distance >= 0.0f)) {
            const float t = current_distance / (current_distance - next_distance);
            out.vertices[out.count++] = LerpVertex(current, next, t);
        }
    }
    return out;
}


/// Twice the signed area of the triangle formed by (a, b, p).
float Edge(const ScreenVertex& a, const ScreenVertex& b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

bool ComparePasses(CompareOp op, float src, float dst) {
    switch (op) {
    case CompareOp::Never: return false;
    case CompareOp::Less: return src < dst;
    case CompareOp::Equal: return src == dst;
    case CompareOp::LessEqual: return src <= dst;
    case CompareOp::Greater: return src > dst;
    case CompareOp::NotEqual: return src != dst;
    case CompareOp::GreaterEqual: return src >= dst;
    case CompareOp::Always: return true;
    }
    return false;
}

float FactorValue(BlendFactor factor, const Vec4& src, const Vec4& dst, int channel) {
    switch (factor) {
    case BlendFactor::Zero: return 0.0f;
    case BlendFactor::One: return 1.0f;
    case BlendFactor::SrcAlpha: return src[3];
    case BlendFactor::OneMinusSrcAlpha: return 1.0f - src[3];
    case BlendFactor::DstAlpha: return dst[3];
    case BlendFactor::OneMinusDstAlpha: return 1.0f - dst[3];
    case BlendFactor::SrcColor: return src[channel];
    case BlendFactor::OneMinusSrcColor: return 1.0f - src[channel];
    case BlendFactor::DstColor: return dst[channel];
    case BlendFactor::OneMinusDstColor: return 1.0f - dst[channel];
    }
    return 1.0f;
}

float ApplyBlendOp(BlendOp op, float a, float b) {
    switch (op) {
    case BlendOp::Add: return a + b;
    case BlendOp::Subtract: return a - b;
    case BlendOp::ReverseSubtract: return b - a;
    case BlendOp::Min: return a < b ? a : b;
    case BlendOp::Max: return a > b ? a : b;
    }
    return a;
}

Vec4 Blend(const render::BlendState& state, const Vec4& src, const Vec4& dst) {
    if (!state.enable) {
        return src;
    }
    Vec4 out{};
    for (int channel = 0; channel < 3; ++channel) {
        const float s = src[channel] * FactorValue(state.src_color, src, dst, channel);
        const float d = dst[channel] * FactorValue(state.dst_color, src, dst, channel);
        out[channel] = ApplyBlendOp(state.color_op, s, d);
    }
    const float s = src[3] * FactorValue(state.src_alpha, src, dst, 3);
    const float d = dst[3] * FactorValue(state.dst_alpha, src, dst, 3);
    out[3] = ApplyBlendOp(state.alpha_op, s, d);
    return out;
}

bool ReadAttribute(GpuMemoryManager& memory, const DrawDesc& draw,
                   const render::VertexAttribute& attribute, std::uint32_t vertex, Vec4& out) {
    if (attribute.buffer_index >= draw.vertex_buffers.size()) {
        return false;
    }
    const BufferBinding& buffer = draw.vertex_buffers[attribute.buffer_index];
    const surface::FormatInfo info = surface::DecodeFormat(attribute.format);
    if (info.bytes_per_pixel == 0) {
        return false;
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(attribute.offset) +
                                 static_cast<std::uint64_t>(attribute.stride) * vertex;
    if (offset + info.bytes_per_pixel > buffer.size) {
        return false;
    }
    const std::uint8_t* bytes = memory.peek(buffer.pa + offset, info.bytes_per_pixel);
    if (bytes == nullptr) {
        return false;
    }
    out = surface::DecodeTexel(info, bytes);
    return true;
}

}  // namespace

Rasterizer::Rasterizer(const DrawDesc& draw, GpuMemoryManager& memory, ThreadPool& pool)
    : draw_(draw), memory_(memory), pool_(pool) {}

RenderResult Rasterizer::Draw() {
    if (draw_.color.format == surface::SurfaceFormat::Unknown) {
        return RenderResult::Fail(RenderStatus::BadTarget, "colour target has no format");
    }
    if (draw_.vertex_shader == nullptr || draw_.fragment_shader == nullptr) {
        return RenderResult::Fail(RenderStatus::ShaderError,
                                  "a vertex and a fragment shader are both required");
    }
    if (draw_.topology != render::PrimitiveTopology::Triangles) {
        return RenderResult::Fail(RenderStatus::UnsupportedState,
                                  "only a triangle list is implemented");
    }

    SoftwareRenderTarget colour(memory_, draw_.color);
    if (!colour.valid()) {
        return RenderResult::Fail(RenderStatus::BadTarget,
                                  "colour target is not mapped guest memory");
    }

    const bool want_depth = draw_.depth.has_value() && draw_.depth_state.test_enable;
    SoftwareRenderTarget depth_target(memory_, draw_.depth.value_or(render::RenderTarget{}));
    if (want_depth && !depth_target.valid()) {
        return RenderResult::Fail(RenderStatus::BadTarget,
                                  "depth target is not mapped guest memory");
    }

    if (draw_.clear.enabled) {
        colour.Clear(draw_.clear);
        if (want_depth) {
            depth_target.Clear(draw_.clear);
        }
    }

    // ---- assemble the vertex index list --------------------------------
    const std::uint32_t count =
        draw_.index_type == render::IndexType::None ? draw_.vertex_count : draw_.index_count;
    if (count == 0) {
        return colour.Flush() ? RenderResult::Good()
                              : RenderResult::Fail(RenderStatus::BadTarget,
                                                   "colour target writeback failed");
    }

    std::vector<std::uint32_t> indices(count);
    if (draw_.index_type == render::IndexType::None) {
        for (std::uint32_t i = 0; i < count; ++i) {
            indices[i] = draw_.first_vertex + i;
        }
    } else {
        const std::uint32_t element = draw_.index_type == render::IndexType::UInt16 ? 2u : 4u;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint64_t offset =
                static_cast<std::uint64_t>(draw_.first_index + i) * element;
            if (offset + element > draw_.index.size) {
                return RenderResult::Fail(RenderStatus::BadGeometry, "index buffer overrun");
            }
            const std::uint8_t* bytes = memory_.peek(draw_.index.pa + offset, element);
            if (bytes == nullptr) {
                return RenderResult::Fail(RenderStatus::BadGeometry,
                                          "index buffer is not mapped");
            }
            if (element == 2) {
                std::uint16_t value = 0;
                std::memcpy(&value, bytes, sizeof(value));
                indices[i] = value;
            } else {
                std::uint32_t value = 0;
                std::memcpy(&value, bytes, sizeof(value));
                indices[i] = value;
            }
        }
    }

    // ---- vertex stage ---------------------------------------------------
    const shader::Module& vs = *draw_.vertex_shader;
    if (vs.outputs.empty()) {
        return RenderResult::Fail(RenderStatus::ShaderError,
                                  "vertex shader declares no position output");
    }

    std::vector<ShadedVertex> shaded(count);
    shader::ShaderIo vs_in;
    vs_in.inputs.resize(vs.inputs.size());
    vs_in.constants = &draw_.constants;
    vs_in.textures = &draw_.textures;
    vs_in.memory = &memory_;
    shader::ShaderIo vs_out;
    vs_out.outputs.resize(vs.outputs.size());

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t vertex = indices[i] + static_cast<std::uint32_t>(draw_.vertex_offset);

        for (std::size_t slot = 0; slot < vs.inputs.size(); ++slot) {
            const render::VertexAttribute* attribute = nullptr;
            for (const auto& candidate : draw_.attributes) {
                if (candidate.location == vs.inputs[slot].location) {
                    attribute = &candidate;
                    break;
                }
            }
            if (attribute == nullptr) {
                return RenderResult::Fail(RenderStatus::ShaderError,
                                          "vertex input has no matching attribute");
            }
            if (!ReadAttribute(memory_, draw_, *attribute, vertex, vs_in.inputs[slot])) {
                return RenderResult::Fail(RenderStatus::BadGeometry,
                                          "vertex attribute read failed");
            }
        }
        vs_in.special[static_cast<std::size_t>(shader::Special::VertexIndex)] = {
            static_cast<float>(vertex), 0.0f, 0.0f, 0.0f};

        const shader::InterpResult result = shader::Interpret(vs, vs_in, vs_out);
        if (!result.ok()) {
            return RenderResult::Fail(RenderStatus::ShaderError, result.error);
        }
        shaded[i].clip = vs_out.outputs[0];
        shaded[i].varyings.assign(vs_out.outputs.begin() + 1, vs_out.outputs.end());
    }

    // ---- fragment input mapping ----------------------------------------
    const shader::Module& fs = *draw_.fragment_shader;
    std::vector<int> fragment_to_varying(fs.inputs.size(), -1);
    for (std::size_t slot = 0; slot < fs.inputs.size(); ++slot) {
        for (std::size_t output = 1; output < vs.outputs.size(); ++output) {
            if (vs.outputs[output].location == fs.inputs[slot].location) {
                fragment_to_varying[slot] = static_cast<int>(output - 1);
                break;
            }
        }
    }

    // The fragment-shader state is per band worker, not per draw: the vectors are
    // sized once and reused across that band's pixels, which is what keeps the
    // pixel loop from allocating (307200 pixels used to mean 600k heap
    // allocations to run a shader of a handful of instructions).

    const auto to_screen = [&](const Vec4& clip) -> std::optional<ScreenVertex> {
        // Nothing behind the near plane reaches here: the triangle was clipped
        // before this ran. The check stays as a guard, not as the mechanism.
        if (!(clip[3] > 0.0f)) {
            return std::nullopt;
        }
        const float inv_w = 1.0f / clip[3];
        const float ndc_x = clip[0] * inv_w;
        const float ndc_y = clip[1] * inv_w;
        const float ndc_z = clip[2] * inv_w;
        ScreenVertex vertex;
        vertex.x = draw_.viewport.x + (ndc_x * 0.5f + 0.5f) * draw_.viewport.width;
        // The viewport flips Y so that NDC +Y is up and screen Y is down.
        vertex.y = draw_.viewport.y + (0.5f - ndc_y * 0.5f) * draw_.viewport.height;
        vertex.z = draw_.viewport.min_depth +
                   ndc_z * (draw_.viewport.max_depth - draw_.viewport.min_depth);
        vertex.inv_w = inv_w;
        return vertex;
    };

    /// One triangle of a clipped polygon, restricted to the rows [band_begin,
    /// band_end). A target or shader failure comes back out; a degenerate or
    /// culled triangle is simply not drawn. The fragment-shader IO is the caller's
    /// because each band worker needs its own.
    const auto rasterize = [&](const ShadedVertex& a, const ShadedVertex& b,
                               const ShadedVertex& c, std::int32_t band_begin,
                               std::int32_t band_end, shader::ShaderIo& fs_in,
                               shader::ShaderIo& fs_out) -> RenderResult {
        const auto s0 = to_screen(a.clip);
        const auto s1 = to_screen(b.clip);
        const auto s2 = to_screen(c.clip);
        if (!s0 || !s1 || !s2) {
            return RenderResult::Good();
        }

        const float area = Edge(*s0, *s1, s2->x, s2->y);
        if (std::fabs(area) < 1e-9f) {
            return RenderResult::Good();  // degenerate
        }

        // Convention: with the Y-flip above, an NDC counter-clockwise triangle
        // has a negative signed area.
        const bool front_facing =
            draw_.raster.front_face == render::FrontFace::CounterClockwise ? (area < 0.0f)
                                                                          : (area > 0.0f);
        if ((draw_.raster.cull == render::CullMode::Front && front_facing) ||
            (draw_.raster.cull == render::CullMode::Back && !front_facing)) {
            return RenderResult::Good();
        }

        std::int32_t min_x = static_cast<std::int32_t>(
            std::floor(std::min({s0->x, s1->x, s2->x})));
        std::int32_t max_x = static_cast<std::int32_t>(
            std::ceil(std::max({s0->x, s1->x, s2->x})));
        std::int32_t min_y = static_cast<std::int32_t>(
            std::floor(std::min({s0->y, s1->y, s2->y})));
        std::int32_t max_y = static_cast<std::int32_t>(
            std::ceil(std::max({s0->y, s1->y, s2->y})));

        if (draw_.raster.scissor_enable) {
            min_x = std::max(min_x, draw_.scissor.x);
            min_y = std::max(min_y, draw_.scissor.y);
            max_x = std::min(max_x, draw_.scissor.x + static_cast<std::int32_t>(draw_.scissor.width));
            max_y = std::min(max_y, draw_.scissor.y + static_cast<std::int32_t>(draw_.scissor.height));
        }
        min_x = std::max(min_x, 0);
        min_y = std::max(min_y, 0);
        max_x = std::min(max_x, static_cast<std::int32_t>(colour.width()));
        max_y = std::min(max_y, static_cast<std::int32_t>(colour.height()));
        // The band is what makes the parallel split safe: this worker owns these
        // rows and no other worker writes them.
        min_y = std::max(min_y, band_begin);
        max_y = std::min(max_y, band_end);

        for (std::int32_t y = min_y; y < max_y; ++y) {
            for (std::int32_t x = min_x; x < max_x; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;

                const float w0 = Edge(*s1, *s2, px, py);
                const float w1 = Edge(*s2, *s0, px, py);
                const float w2 = Edge(*s0, *s1, px, py);
                if (area > 0.0f ? (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                                : (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)) {
                    continue;
                }

                const float l0 = w0 / area;
                const float l1 = w1 / area;
                const float l2 = w2 / area;
                const float depth = l0 * s0->z + l1 * s1->z + l2 * s2->z;

                if (want_depth) {
                    Vec4 stored{};
                    if (!depth_target.Load(static_cast<std::uint32_t>(x),
                                           static_cast<std::uint32_t>(y), stored)) {
                        return RenderResult::Fail(RenderStatus::BadTarget,
                                                  "depth target read failed");
                    }
                    if (!ComparePasses(draw_.depth_state.compare, depth, stored[0])) {
                        continue;
                    }
                }

                // Perspective-correct interpolation.
                const float inv_w = l0 * s0->inv_w + l1 * s1->inv_w + l2 * s2->inv_w;
                const float inv_w_safe = inv_w != 0.0f ? inv_w : 1.0f;

                for (std::size_t slot = 0; slot < fs.inputs.size(); ++slot) {
                    const int varying = fragment_to_varying[slot];
                    if (varying < 0) {
                        return RenderResult::Fail(
                            RenderStatus::ShaderError,
                            "fragment input has no matching vertex output");
                    }
                    const Vec4& v0 = a.varyings[static_cast<std::size_t>(varying)];
                    const Vec4& v1 = b.varyings[static_cast<std::size_t>(varying)];
                    const Vec4& v2 = c.varyings[static_cast<std::size_t>(varying)];
                    Vec4 interpolated{};
                    for (int channel = 0; channel < 4; ++channel) {
                        interpolated[channel] =
                            (l0 * v0[channel] * s0->inv_w + l1 * v1[channel] * s1->inv_w +
                             l2 * v2[channel] * s2->inv_w) /
                            inv_w_safe;
                    }
                    fs_in.inputs[slot] = interpolated;
                }
                fs_in.special[static_cast<std::size_t>(shader::Special::FragCoord)] = {
                    px, py, depth, inv_w_safe};
                fs_in.special[static_cast<std::size_t>(shader::Special::FrontFacing)] = {
                    front_facing ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};

                // A shader that does not write every output must not leave the
                // previous pixel's values behind.
                std::fill(fs_out.outputs.begin(), fs_out.outputs.end(), Vec4{0, 0, 0, 0});
                const shader::InterpResult result = shader::Interpret(fs, fs_in, fs_out);
                if (!result.ok()) {
                    return RenderResult::Fail(RenderStatus::ShaderError, result.error);
                }
                if (result.killed) {
                    continue;
                }

                const Vec4 source = fs_out.outputs.empty() ? Vec4{0, 0, 0, 0} : fs_out.outputs[0];

                Vec4 existing{};
                if (!colour.Load(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                 existing)) {
                    return RenderResult::Fail(RenderStatus::BadTarget,
                                              "colour target read failed");
                }
                Vec4 result_colour = Blend(draw_.blend, source, existing);
                for (int channel = 0; channel < 4; ++channel) {
                    if ((draw_.blend.write_mask & (1u << channel)) == 0) {
                        result_colour[channel] = existing[channel];
                    }
                }
                if (!colour.Store(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                  result_colour)) {
                    return RenderResult::Fail(RenderStatus::BadTarget,
                                              "colour target write failed");
                }
                if (want_depth && draw_.depth_state.write_enable) {
                    depth_target.Store(static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), Vec4{depth, 0, 0, 1});
                }
            }
        }
        return RenderResult::Good();
    };

    // Clip every triangle once, up front. Cutting is cheap but not free, and each
    // band worker would otherwise repeat it for the same triangle.
    struct FlatTriangle {
        ShadedVertex vertices[3];
    };
    std::vector<FlatTriangle> triangles;
    const std::uint32_t triangle_count = count / 3;
    for (std::uint32_t triangle = 0; triangle < triangle_count; ++triangle) {
        // Cut first, then rasterize: a triangle with any corner behind the near
        // plane used to be dropped whole, which is why a ground quad under the
        // camera vanished.
        const ClippedPolygon clipped = ClipToNearPlane(
            shaded[triangle * 3 + 0], shaded[triangle * 3 + 1], shaded[triangle * 3 + 2]);
        // Fan-triangulate whatever survived the cut.
        for (std::size_t fan = 1; fan + 1 < clipped.count; ++fan) {
            FlatTriangle flat;
            flat.vertices[0] = clipped.vertices[0];
            flat.vertices[1] = clipped.vertices[fan];
            flat.vertices[2] = clipped.vertices[fan + 1];
            triangles.push_back(std::move(flat));
        }
    }

    // Shade bands of rows, one worker each. Each band owns a disjoint row range of
    // both targets, so the per-texel Load/Store needs no lock, and within a band
    // the triangle order is what a single-threaded rasterizer would have used, so
    // blending and the depth test see the same sequence and the result is
    // bit-identical.
    const std::uint32_t rows = colour.height();
    const std::size_t parallelism = pool_.parallelism();
    const bool worth_splitting = parallelism > 1 && rows >= 64 && !triangles.empty();
    // Several bands per worker, so an uneven scene still balances out. The bands
    // re-iterate the triangle list, which is cheap next to shading a row.
    const std::size_t band_count =
        worth_splitting ? std::min<std::size_t>(parallelism * 4, rows) : 1;
    const std::uint32_t band_rows =
        static_cast<std::uint32_t>((rows + band_count - 1) / band_count);

    std::mutex failure_mutex;
    RenderResult failure;
    pool_.Run(band_count, [&](std::size_t band) {
        shader::ShaderIo fs_in;
        fs_in.inputs.resize(fs.inputs.size());
        fs_in.constants = &draw_.constants;
        fs_in.textures = &draw_.textures;
        fs_in.memory = &memory_;
        shader::ShaderIo fs_out;
        fs_out.outputs.resize(fs.outputs.size());

        const auto begin = static_cast<std::int32_t>(band * band_rows);
        const auto end = static_cast<std::int32_t>(
            std::min<std::uint32_t>(rows, static_cast<std::uint32_t>((band + 1) * band_rows)));
        if (begin >= end) {
            return;
        }
        for (const FlatTriangle& triangle : triangles) {
            const RenderResult result = rasterize(triangle.vertices[0], triangle.vertices[1],
                                                  triangle.vertices[2], begin, end, fs_in, fs_out);
            if (!result.ok()) {
                std::lock_guard<std::mutex> lock(failure_mutex);
                if (failure.ok()) {
                    failure = result;
                }
                return;
            }
        }
    });
    if (!failure.ok()) {
        return failure;
    }

    if (!colour.Flush()) {
        return RenderResult::Fail(RenderStatus::BadTarget, "colour target writeback failed");
    }
    if (want_depth && !depth_target.Flush()) {
        return RenderResult::Fail(RenderStatus::BadTarget, "depth target writeback failed");
    }
    return RenderResult::Good();
}

}  // namespace zlong::gpu::software
