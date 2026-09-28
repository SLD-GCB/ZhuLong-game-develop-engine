// 烛龙 (ZhuLong) - 故宫: a palace, generated rather than loaded.
//
// The renderer has no instancing and no asset loader, so a colonnade of thirty columns as thirty
// nodes is thirty draws, and thirty identical columns *inside one mesh* is one. Everything here
// is therefore baked by material: one `Builder` per material accumulates every box, panel and
// column that material wears across the whole building, and comes out as a single mesh. The whole
// palace is a dozen draws.
//
// It is also why the shapes are worth doing properly rather than out of boxes: a Chinese roof is
// a curve, not a triangle, and the curve is what makes the building recognisable.

#pragma once

#include <cstdint>
#include <vector>

#include "zlong/engine/math.h"
#include "zlong/engine/mesh.h"
#include "zlong/engine/scene.h"

namespace zlong::gugong {

using engine::Vec3;

/// Which half of the palace the camera looks at. One world, two views: the courtyard in front of
/// the hall, and the throne room inside it.
enum class View { Courtyard, ThroneRoom };

/// Accumulates one material's worth of geometry. A mesh takes one material, so one of these per
/// material is what turns a building into a handful of draws.
class Builder {
public:
    /// A box between two corners. `uv_metres` is how many metres one tile of the texture covers,
    /// so a texture repeats at a fixed real size whatever the box is.
    void Box(const Vec3& min, const Vec3& max, float uv_metres = 2.0f);

    /// One quad, corners counter-clockwise seen from outside. uv runs along the first two edges,
    /// in metres, so a tiled material keeps its scale on a slope.
    void Panel(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float uv_metres = 2.0f);

    /// The same, with the uv given instead of measured: what a sky wants, where the texture is
    /// the whole gradient rather than something that tiles.
    void PanelUv(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                 const float uv[4][2]);

    /// An upright cylinder from y0 to y1, centred on (x, z).
    void Column(float x, float z, float radius, float y0, float y1, float uv_metres = 1.0f,
                int sides = 12);

    /// The same, lying along +Z: for beams and rails that run the other way.
    void BeamX(float y, float z, float x0, float x1, float half_height, float half_depth);

    bool empty() const noexcept { return vertices_.empty(); }
    std::size_t size() const noexcept { return vertices_.size(); }

    /// The finished mesh. Consumes the builder's vertices, so it can only be taken once.
    engine::Mesh Take();

private:
    void Vertex(const Vec3& position, const Vec3& normal, const Vec3& tangent, float u, float v);

    std::vector<engine::MeshVertex> vertices_;
    std::vector<std::uint16_t> indices_;
};

/// Append the whole palace to `scene`: the ground, the terrace, the hall with its colonnade, doors,
/// brackets and roof, the walls, and the trees. Sets the lights, the shadow map and the camera for
/// `view`, because the two views want different framing and different light.
void Build(engine::Scene& scene, View view);

}  // namespace zlong::gugong
