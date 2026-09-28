// 烛龙 (ZhuLong) - what the game is made of, in geometry.
//
// Everything here is procedural -- there is no loader in the engine and no asset file in the
// project -- so the board is a generated texture and every piece is a traditional Chinese
// chess piece: **a boxwood disc with its character on the face**. Two primitives each, and
// the character is the one thing that has to be drawn rather than generated, so it comes
// from `glyph.cpp` and the platform's text API.
//
// Red and black wear the same boxwood. What tells them apart is the ink and the ring, which
// is how a real set works, and it is legible because the camera looks down on the faces.

#pragma once

#include <cstdint>
#include <vector>

#include "rules.h"
#include "zlong/engine/math.h"
#include "zlong/engine/mesh.h"
#include "zlong/engine/scene.h"

namespace zlong::xiangqi {

/// Board geometry. A square is one unit; the slab is a little larger than the nine by ten
/// intersections it carries. A piece is a disc a little under one square across.
inline constexpr float kSquare = 1.0f;
inline constexpr float kSlabWidth = 11.0f;
inline constexpr float kSlabDepth = 12.0f;
inline constexpr float kPieceRadius = 0.42f;
inline constexpr float kPieceHeight = 0.18f;

/// Where a square sits in the world. The board lies in the XZ plane with red at -Z, and a
/// piece's node is placed with y = 0 at this point.
engine::Vec3 SquareCentre(Square square) noexcept;

/// Where the game's procedural meshes and materials landed in the scene. A scene is a list,
/// so building the game means appending to it and keeping the indices.
struct Assets {
    std::uint32_t box = 0;
    std::uint32_t cylinder = 0;
    std::uint32_t plane = 0;
    /// The floor: the same quad with its texture repeated, so the boards lay at a sane size.
    std::uint32_t tiled_plane = 0;
    /// A flat face, for the character on a piece.
    std::uint32_t disc = 0;
    /// The shot a cannon fires.
    std::uint32_t sphere = 0;

    std::uint32_t slab_material = 0;
    std::uint32_t board_material = 0;
    /// Boxwood: the body of every piece, both sides.
    std::uint32_t piece_material = 0;
    /// One per kind and side -- the face with its character and its ring, in the side's ink.
    std::uint32_t face_red[kKindCount] = {};
    std::uint32_t face_black[kKindCount] = {};
    std::uint32_t cursor = 0;
    std::uint32_t move_marker = 0;
    std::uint32_t capture_marker = 0;
    /// Under the two squares of the move that was just played.
    std::uint32_t trail = 0;
    std::uint32_t projectile = 0;

    // --- the room the board stands in --------------------------------------
    std::uint32_t floor_material = 0;
    std::uint32_t rug_material = 0;
    std::uint32_t table_material = 0;
    std::uint32_t wall_material = 0;
};

/// Append the meshes, the materials and the way the board is lit. Also sets the camera and
/// the background, because a game has one look and this is where it is decided.
Assets AddAssets(engine::Scene& scene);

/// One primitive of a figure, in the piece's own space with y = 0 at its base.
struct FigurePart {
    std::uint32_t mesh = 0;
    std::uint32_t material = 0;
    engine::Mat4 local{};
};

/// The two primitives a piece is built from: the disc, and the inscribed face on top of it.
/// `red` picks which side's ink the character is cut in.
std::vector<FigurePart> Figure(Kind kind, bool red, const Assets& assets);

/// Everything around the board: the floor, the rug, the table it stands on and the three
/// walls the camera can see. Static -- the presenter adds these once and never touches them
/// again -- and returned in the same shape as a figure so that adding it costs one loop.
std::vector<FigurePart> Room(const Assets& assets);

}  // namespace zlong::xiangqi
