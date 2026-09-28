#include "figures.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "glyph.h"

namespace zlong::xiangqi {

namespace {

using engine::Scale;
using engine::Translation;

/// Where the nine by ten intersections fall in the slab's 0..1 uv, given that the slab is
/// kSlabWidth by kSlabDepth units and the pieces stand on unit pitch.
constexpr float kU0 = 0.5f - 4.0f / kSlabWidth;
constexpr float kU1 = 0.5f + 4.0f / kSlabWidth;
constexpr float kV0 = 0.5f - 4.5f / kSlabDepth;
constexpr float kV1 = 0.5f + 4.5f / kSlabDepth;

float FileU(int file) { return kU0 + (kU1 - kU0) * static_cast<float>(file) / 8.0f; }
float RankV(int rank) { return kV0 + (kV1 - kV0) * static_cast<float>(rank) / 9.0f; }

/// The character cut into a piece's face. Red and black use different words for four of the
/// seven kinds, which is the rest of what tells the sides apart.
const wchar_t* Glyph(Kind kind, bool red) {
    static const wchar_t* const kRed[kKindCount] = {L"帅", L"仕", L"相", L"马", L"车", L"炮",
                                                    L"兵"};
    static const wchar_t* const kBlack[kKindCount] = {L"将", L"士", L"象", L"马", L"车", L"炮",
                                                      L"卒"};
    return (red ? kRed : kBlack)[static_cast<int>(kind)];
}

/// A drawing surface for the board texture, in pixels. Lines are drawn at a supersampled
/// resolution and the result is boxed down, which is what keeps a nine-by-ten grid from
/// coming out stair-stepped.
struct Canvas {
    int size = 0;
    std::vector<std::uint8_t> pixels;

    Canvas(int side, std::uint8_t r, std::uint8_t g, std::uint8_t b)
        : size(side), pixels(static_cast<std::size_t>(side) * side * 4) {
        for (int index = 0; index < side * side; ++index) {
            pixels[static_cast<std::size_t>(index) * 4 + 0] = r;
            pixels[static_cast<std::size_t>(index) * 4 + 1] = g;
            pixels[static_cast<std::size_t>(index) * 4 + 2] = b;
            pixels[static_cast<std::size_t>(index) * 4 + 3] = 255;
        }
    }

    void Plot(int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (x < 0 || y < 0 || x >= size || y >= size) {
            return;
        }
        const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
        pixels[at + 0] = r;
        pixels[at + 1] = g;
        pixels[at + 2] = b;
    }

    void Rect(int x0, int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                Plot(x, y, r, g, b);
            }
        }
    }

    /// A line between two points, drawn as small squares along the way so a 45-degree
    /// diagonal comes out as a line rather than a dotted heap.
    void Line(float ax, float ay, float bx, float by, int thickness, std::uint8_t r,
              std::uint8_t g, std::uint8_t b) {
        const int steps =
            static_cast<int>(std::max(std::fabs(bx - ax), std::fabs(by - ay))) + 1;
        for (int step = 0; step <= steps; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const int x = static_cast<int>(std::lround(ax + (bx - ax) * t));
            const int y = static_cast<int>(std::lround(ay + (by - ay) * t));
            Rect(x - thickness, y - thickness, x + thickness, y + thickness, r, g, b);
        }
    }
};

/// A hash-noise value field: cheap, deterministic, and smooth enough that its gradient reads
/// as a material rather than as static.
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

/// A per-texel painter: `paint(u, v, rgb)` for u and v in 0..1. Every surface in the room is
/// a function like this rather than a file, because there is nothing to load.
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

/// The board itself: nine files, ten ranks, the river with its two outer lines running
/// through, and the two palaces. Drawn rather than loaded, like everything else here.
engine::Texture MakeBoardTexture(std::uint32_t size) {
    constexpr int kSupersample = 2;
    const int big = static_cast<int>(size) * kSupersample;
    Canvas canvas(big, 198, 158, 102);

    // A wash over the river band, so the two halves read as two halves.
    const int river_top = static_cast<int>(RankV(4) * big);
    const int river_bottom = static_cast<int>(RankV(5) * big);
    canvas.Rect(0, river_top, big - 1, river_bottom, 184, 144, 92);

    const std::uint8_t lr = 54, lg = 36, lb = 20;
    const int thin = kSupersample;
    const int thick = kSupersample * 2;

    const auto ux = [&](int file) { return FileU(file) * static_cast<float>(big); };
    const auto vy = [&](int rank) { return RankV(rank) * static_cast<float>(big); };

    for (int rank = 0; rank < kRanks; ++rank) {
        const float y = vy(rank);
        canvas.Line(ux(0), y, ux(8), y, thin, lr, lg, lb);
    }
    for (int file = 0; file < kFiles; ++file) {
        const float x = ux(file);
        if (file == 0 || file == 8) {
            canvas.Line(x, vy(0), x, vy(9), thick, lr, lg, lb);
        } else {
            canvas.Line(x, vy(0), x, vy(4), thin, lr, lg, lb);
            canvas.Line(x, vy(5), x, vy(9), thin, lr, lg, lb);
        }
    }
    for (const int end : {0, 7}) {
        canvas.Line(ux(3), vy(end), ux(5), vy(end + 2), thin, lr, lg, lb);
        canvas.Line(ux(5), vy(end), ux(3), vy(end + 2), thin, lr, lg, lb);
    }

    engine::Texture texture;
    texture.width = size;
    texture.height = size;
    texture.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < kSupersample; ++dy) {
                for (int dx = 0; dx < kSupersample; ++dx) {
                    const std::size_t at =
                        (static_cast<std::size_t>(y * kSupersample + dy) * big +
                         (x * kSupersample + dx)) *
                        4;
                    for (int channel = 0; channel < 4; ++channel) {
                        sum[channel] += canvas.pixels[at + channel];
                    }
                }
            }
            const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                texture.rgba[at + channel] =
                    static_cast<std::uint8_t>(sum[channel] / (kSupersample * kSupersample));
            }
        }
    }
    return texture;
}

/// A gentle grain, so the board's surface catches the light unevenly instead of reading as a
/// flat colour. Low amplitude on purpose: the sampler is nearest, so anything strong comes
/// out as blotches.
engine::Texture MakeGrainNormalTexture(std::uint32_t size) {
    engine::Texture texture;
    texture.width = size;
    texture.height = size;
    texture.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);

    const auto height = [&](int x, int y) {
        const int wrapped_x = (x + static_cast<int>(size)) % static_cast<int>(size);
        const int wrapped_y = (y + static_cast<int>(size)) % static_cast<int>(size);
        const float u = static_cast<float>(wrapped_x) / static_cast<float>(size);
        const float v = static_cast<float>(wrapped_y) / static_cast<float>(size);
        return 0.75f * SmoothNoise(u * 4.0f, v * 4.0f, 11u) +
               0.25f * SmoothNoise(u * 11.0f, v * 11.0f, 29u);
    };

    constexpr float kStrength = 0.08f;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float slope_x =
                (height(static_cast<int>(x) + 1, static_cast<int>(y)) -
                 height(static_cast<int>(x) - 1, static_cast<int>(y))) *
                static_cast<float>(size) / 2.0f * kStrength / 32.0f;
            const float slope_y =
                (height(static_cast<int>(x), static_cast<int>(y) + 1) -
                 height(static_cast<int>(x), static_cast<int>(y) - 1)) *
                static_cast<float>(size) / 2.0f * kStrength / 32.0f;
            const float inverse = 1.0f / std::sqrt(slope_x * slope_x + slope_y * slope_y + 1.0f);
            const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
            texture.rgba[at + 0] = static_cast<std::uint8_t>(
                std::clamp((-slope_x * inverse * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            texture.rgba[at + 1] = static_cast<std::uint8_t>(
                std::clamp((-slope_y * inverse * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            texture.rgba[at + 2] = static_cast<std::uint8_t>(
                std::clamp((inverse * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            texture.rgba[at + 3] = 255;
        }
    }
    return texture;
}

/// Floorboards: bands a shade apart, a seam where two meet, and grain running along them.
engine::Texture MakeFloorTexture(std::uint32_t size, int boards, const float tone[3]) {
    return MakeTexture(size, [boards, tone](float u, float v, float* rgb) {
        // The boards run along u, so v is the axis that bands.
        const float along = v * static_cast<float>(boards);
        const int index = static_cast<int>(std::floor(along));
        const float within = along - static_cast<float>(index);
        float shade = 0.88f + 0.24f * Hash(index, 7, 1234u);
        if (std::min(within, 1.0f - within) < 0.03f) {
            shade *= 0.55f;  // the seam
        }
        const float grain = SmoothNoise(u * 22.0f, along * 2.5f, 77u) - 0.5f;
        const float wear = SmoothNoise(u * 3.0f, v * 3.0f, 991u) - 0.5f;
        shade *= 1.0f + grain * 0.18f + wear * 0.16f;
        for (int channel = 0; channel < 3; ++channel) {
            rgb[channel] = tone[channel] * shade;
        }
    });
}

/// A single slab: a tight grain rather than boards, for a tabletop cut from one piece.
engine::Texture MakeTableTexture(std::uint32_t size, const float tone[3]) {
    return MakeTexture(size, [tone](float u, float v, float* rgb) {
        const float grain = SmoothNoise(u * 7.0f, v * 30.0f, 5u) - 0.5f;
        const float figure = SmoothNoise(u * 4.0f, v * 4.0f, 19u) - 0.5f;
        const float shade = 1.0f + grain * 0.24f + figure * 0.16f;
        for (int channel = 0; channel < 3; ++channel) {
            rgb[channel] = tone[channel] * shade;
        }
    });
}

/// Plaster: two scales of mottling and nothing else. A room's wall is the one surface that
/// should stay quiet.
engine::Texture MakePlasterTexture(std::uint32_t size, const float tone[3]) {
    return MakeTexture(size, [tone](float u, float v, float* rgb) {
        const float broad = SmoothNoise(u * 4.0f, v * 4.0f, 31u) - 0.5f;
        const float fine = SmoothNoise(u * 17.0f, v * 17.0f, 53u) - 0.5f;
        const float shade = 0.95f + broad * 0.14f + fine * 0.05f;
        for (int channel = 0; channel < 3; ++channel) {
            rgb[channel] = tone[channel] * shade;
        }
    });
}

/// A runner: a deep field, a cream border, and a darker edge outside that.
engine::Texture MakeRugTexture(std::uint32_t size) {
    return MakeTexture(size, [](float u, float v, float* rgb) {
        const float field[3] = {0.33f, 0.13f, 0.14f};
        const float border[3] = {0.62f, 0.55f, 0.40f};
        const float outside[3] = {0.17f, 0.08f, 0.09f};
        const float edge = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
        const float* tone = field;
        if (edge < 0.035f) {
            tone = outside;
        } else if (edge < 0.10f) {
            tone = border;
        } else if (edge < 0.115f) {
            tone = outside;
        }
        const float weave = SmoothNoise(u * 70.0f, v * 70.0f, 11u) - 0.5f;
        const float shade = 1.0f + weave * 0.12f;
        for (int channel = 0; channel < 3; ++channel) {
            rgb[channel] = tone[channel] * shade;
        }
    });
}

}  // namespace

engine::Vec3 SquareCentre(Square square) noexcept {
    return engine::Vec3{static_cast<float>(square.file - 4) * kSquare, 0.0f,
                        static_cast<float>(square.rank) * kSquare - 4.5f * kSquare};
}

Assets AddAssets(engine::Scene& scene) {
    Assets assets;
    assets.box = static_cast<std::uint32_t>(scene.meshes.size());
    scene.meshes.push_back(engine::MakeBox());
    assets.cylinder = static_cast<std::uint32_t>(scene.meshes.size());
    scene.meshes.push_back(engine::MakeCylinder(28));
    assets.plane = static_cast<std::uint32_t>(scene.meshes.size());
    scene.meshes.push_back(engine::MakeGrid(1));
    assets.tiled_plane = static_cast<std::uint32_t>(scene.meshes.size());
    scene.meshes.push_back(engine::MakeGrid(1, 9.0f));
    assets.disc = static_cast<std::uint32_t>(scene.meshes.size());
    scene.meshes.push_back(engine::MakeDisc(36));
    assets.sphere = static_cast<std::uint32_t>(scene.meshes.size());
    scene.meshes.push_back(engine::MakeSphere(8, 16));

    const auto add_material = [&](const engine::Material& material) {
        scene.materials.push_back(material);
        return static_cast<std::uint32_t>(scene.materials.size() - 1);
    };
    const auto add_texture = [&](engine::Texture texture) {
        scene.textures.push_back(std::move(texture));
        return static_cast<std::uint32_t>(scene.textures.size() - 1);
    };

    engine::Material slab;
    slab.tint = {0.30f, 0.19f, 0.11f, 1.0f};
    slab.specular = 0.10f;
    assets.slab_material = add_material(slab);

    add_texture(MakeBoardTexture(1024));
    add_texture(MakeGrainNormalTexture(256));
    engine::Material board;
    board.tint = {1.0f, 1.0f, 1.0f, 1.0f};
    board.specular = 0.16f;
    board.texture = 0;
    board.normal_texture = 1;
    assets.board_material = add_material(board);

    // Boxwood, for every piece's body. Both sides share it on purpose: a real set does, and
    // the ink and the ring are what distinguish them.
    engine::Material piece;
    piece.tint = {0.86f, 0.79f, 0.64f, 1.0f};
    piece.specular = 0.22f;
    assets.piece_material = add_material(piece);

    // The faces: one per kind and side, each a drawn character. This is the only place in the
    // project that needs a font, and it is the host that has one.
    constexpr std::uint32_t kRedInk = 0xB01E1E;
    constexpr std::uint32_t kRedRing = 0xA63A32;
    constexpr std::uint32_t kBlackInk = 0x1E1E24;
    constexpr std::uint32_t kBlackRing = 0x33333C;
    for (int kind = 0; kind < kKindCount; ++kind) {
        engine::Material face;
        face.tint = {1.0f, 1.0f, 1.0f, 1.0f};
        face.specular = 0.14f;
        face.texture = add_texture(MakePieceFace(
            Glyph(static_cast<Kind>(kind), /*red=*/true), kRedInk, kRedRing, 64));
        assets.face_red[kind] = add_material(face);

        engine::Material dark = face;
        dark.texture = add_texture(MakePieceFace(
            Glyph(static_cast<Kind>(kind), /*red=*/false), kBlackInk, kBlackRing, 64));
        assets.face_black[kind] = add_material(dark);
    }

    engine::Material cursor;
    cursor.tint = {1.0f, 0.86f, 0.24f, 1.0f};
    cursor.specular = 0.35f;
    assets.cursor = add_material(cursor);

    engine::Material move_marker;
    move_marker.tint = {0.22f, 0.52f, 0.30f, 1.0f};
    move_marker.specular = 0.22f;
    assets.move_marker = add_material(move_marker);

    engine::Material capture_marker;
    capture_marker.tint = {0.72f, 0.20f, 0.18f, 1.0f};
    capture_marker.specular = 0.30f;
    assets.capture_marker = add_material(capture_marker);

    engine::Material trail;
    trail.tint = {0.34f, 0.38f, 0.46f, 1.0f};
    trail.specular = 0.25f;
    assets.trail = add_material(trail);

    engine::Material projectile;
    projectile.tint = {0.85f, 0.78f, 0.55f, 1.0f};
    projectile.specular = 0.60f;
    assets.projectile = add_material(projectile);

    // --- the room ------------------------------------------------------------
    // The study the board stands in. Every surface here is a painter, and the tones are
    // chosen to sit below the board: the pieces are the picture and the room is the frame.
    const float floor_tone[3] = {0.30f, 0.21f, 0.13f};
    const float table_tone[3] = {0.40f, 0.26f, 0.14f};
    const float wall_tone[3] = {0.56f, 0.51f, 0.45f};

    engine::Material floor;
    floor.tint = {1.0f, 1.0f, 1.0f, 1.0f};
    floor.specular = 0.10f;
    floor.texture = add_texture(MakeFloorTexture(256, 6, floor_tone));
    assets.floor_material = add_material(floor);

    engine::Material rug;
    rug.tint = {1.0f, 1.0f, 1.0f, 1.0f};
    rug.specular = 0.02f;
    rug.texture = add_texture(MakeRugTexture(256));
    assets.rug_material = add_material(rug);

    engine::Material table;
    table.tint = {1.0f, 1.0f, 1.0f, 1.0f};
    table.specular = 0.26f;
    table.texture = add_texture(MakeTableTexture(256, table_tone));
    assets.table_material = add_material(table);

    engine::Material wall;
    wall.tint = {1.0f, 1.0f, 1.0f, 1.0f};
    wall.specular = 0.05f;
    wall.texture = add_texture(MakePlasterTexture(256, wall_tone));
    assets.wall_material = add_material(wall);

    // --- the way it is lit ---------------------------------------------------
    // One warm key from up and behind the camera, one dim cool fill from beyond the board,
    // and enough ambient that the far side of a piece -- or of the room -- is not black. The
    // key casts the shadow, so the pieces sit on the board instead of floating over it.
    engine::Light key;
    key.direction = engine::Vec3{0.30f, 0.86f, -0.42f};
    key.colour = engine::Vec3{0.88f, 0.83f, 0.73f};
    scene.lights.push_back(key);

    engine::Light fill;
    fill.direction = engine::Vec3{-0.55f, 0.28f, 0.50f};
    fill.colour = engine::Vec3{0.16f, 0.19f, 0.26f};
    scene.lights.push_back(fill);

    scene.ambient = engine::Vec3{0.17f, 0.16f, 0.18f};
    scene.background = {0.045f, 0.05f, 0.07f, 1.0f};
    // 512 over the whole room, not over the board: the shadow map is fitted to the scene
    // bounds, so the room diluted it. 1024 was measured at 115 ms a frame against 55 at 512,
    // because the map is a 16 MB float target the pass copies back every frame -- not worth
    // it for a softer edge on a floor nobody looks at.
    scene.shadow_map_size = 512;

    // Red's back rank is at -Z and the camera stands behind it, so the side to move first is
    // the side nearest the player. It sits above the walls and looks down into the room.
    scene.camera.eye = engine::Vec3{0.0f, 18.5f, -13.0f};
    scene.camera.target = engine::Vec3{0.0f, 0.0f, 0.9f};
    scene.camera.up = engine::Vec3{0.0f, 1.0f, 0.0f};
    scene.camera.fov_y_radians = engine::Radians(42.0f);
    scene.camera.far_z = 160.0f;
    return assets;
}

std::vector<FigurePart> Figure(Kind kind, bool red, const Assets& assets) {
    const std::uint32_t face =
        (red ? assets.face_red : assets.face_black)[static_cast<int>(kind)];
    // A boxwood disc, with the inscribed face sitting a hair proud of the rim -- which is both
    // how a real piece is turned and what stops the two coplanar surfaces from z-fighting.
    //
    // The half turn on the face is the camera correction: the game's camera reads a disc's uv
    // reversed on both axes, so a character drawn upright would come out upside down and
    // mirrored. It is invisible on a circle. Note what that makes of black: `Presenter`
    // already turns a black piece to face its opponent, so black's correction and its facing
    // cancel, and black's characters read upright to the *black* player and upside down to us
    // -- which is exactly what a real set does.
    const float radius_scale = kPieceRadius * 2.0f;
    return {
        {assets.cylinder, assets.piece_material,
         Translation(0.0f, kPieceHeight * 0.5f, 0.0f) *
             Scale(radius_scale, kPieceHeight, radius_scale)},
        {assets.disc, face,
         Translation(0.0f, kPieceHeight + 0.006f, 0.0f) * engine::RotationY(3.14159265f) *
             Scale(radius_scale - 0.04f, 1.0f, radius_scale - 0.04f)},
    };
}

std::vector<FigurePart> Room(const Assets& assets) {
    const auto at = [](float x, float y, float z, float sx, float sy, float sz) {
        return Translation(x, y, z) * Scale(sx, sy, sz);
    };
    return {
        // The floor, and the rug a hair above it so the two do not z-fight.
        {assets.tiled_plane, assets.floor_material, at(0.0f, -2.30f, 0.0f, 30.0f, 1.0f, 30.0f)},
        {assets.plane, assets.rug_material, at(0.0f, -2.284f, 0.5f, 20.0f, 1.0f, 22.0f)},
        // The table: its top level with the bottom of the board's slab, so the board rests on
        // it rather than floating a hand's width above.
        {assets.box, assets.table_material, at(0.0f, -0.755f, 0.0f, 14.0f, 0.35f, 15.0f)},
        {assets.box, assets.table_material, at(-6.2f, -1.615f, -6.6f, 0.7f, 1.37f, 0.7f)},
        {assets.box, assets.table_material, at(6.2f, -1.615f, -6.6f, 0.7f, 1.37f, 0.7f)},
        {assets.box, assets.table_material, at(-6.2f, -1.615f, 6.6f, 0.7f, 1.37f, 0.7f)},
        {assets.box, assets.table_material, at(6.2f, -1.615f, 6.6f, 0.7f, 1.37f, 0.7f)},
        // Three walls. The camera stands above their tops, so it is the inside faces that
        // show, and the walls overlap at the corners so no seam of background gets through.
        {assets.box, assets.wall_material, at(0.0f, 2.85f, 15.0f, 30.5f, 10.3f, 0.5f)},
        {assets.box, assets.wall_material, at(-15.0f, 2.85f, 0.25f, 0.5f, 10.3f, 30.5f)},
        {assets.box, assets.wall_material, at(15.0f, 2.85f, 0.25f, 0.5f, 10.3f, 30.5f)},
        // Skirting, which is what says "wall meets floor" rather than "two planes happen to
        // touch".
        {assets.box, assets.table_material, at(0.0f, -2.15f, 14.65f, 30.5f, 0.30f, 0.20f)},
        {assets.box, assets.table_material, at(-14.65f, -2.15f, 0.25f, 0.20f, 0.30f, 30.5f)},
        {assets.box, assets.table_material, at(14.65f, -2.15f, 0.25f, 0.20f, 0.30f, 30.5f)},
        // A screen on the far wall, so the room has a back to it instead of a void. A dark
        // lacquer panel in a wood frame: the kind a study has behind the table.
        {assets.box, assets.table_material, at(0.0f, 0.85f, 14.62f, 9.4f, 4.6f, 0.16f)},
        {assets.box, assets.slab_material, at(0.0f, 0.85f, 14.54f, 8.4f, 3.6f, 0.16f)},
    };
}

}  // namespace zlong::xiangqi
