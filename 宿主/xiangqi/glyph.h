// 烛龙 (ZhuLong) - the character on a piece's face.
//
// The engine has no font and no asset loader, and a Chinese chess piece without its
// character is just a counter. Drawing one is the host's job -- the platform is the only
// thing in the project with a text API -- so it happens here, once per kind and side, into
// an engine::Texture that the face disc wears.
//
// Orientation: the game's camera looks along +Z with its right hand along -X, so a disc
// face's uv comes out reversed on both axes. The character is drawn upright here and the
// piece's face node carries a half turn, which is the same thing and does not depend on
// DrawText laying rotated text out sensibly.

#pragma once

#include <cstdint>

#include "zlong/engine/scene.h"

namespace zlong::xiangqi {

/// A square texture: a boxwood face, a ring in `ring` near the edge, and `glyph` in `ink`
/// filling the middle. Drawn oversized and boxed down, because the engine's sampler is
/// nearest with no mipmaps and a character drawn at its final few dozen pixels would come
/// out ragged.
engine::Texture MakePieceFace(const wchar_t* glyph, std::uint32_t ink, std::uint32_t ring,
                              std::uint32_t size = 64);

}  // namespace zlong::xiangqi
