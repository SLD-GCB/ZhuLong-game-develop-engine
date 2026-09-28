// 烛龙 (ZhuLong) - what a scene IS, as data.
//
// The point of this type is that a scene is not code. `render_scene` used to
// build its four objects inline; anything with more objects than that has to be
// a list, because adding a thousand objects should be a thousand entries and not
// a thousand edits.
//
// It describes everything that exists in the world, seen or heard: meshes and
// materials and nodes for the renderer, sounds and emitters for the sound renderer.
// Both walk the same transform tree, and the listener is the same camera. Nothing
// here knows about the GPU, guest memory, or a device.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "zlong/audio/format.h"
#include "zlong/engine/math.h"
#include "zlong/engine/mesh.h"

namespace zlong::engine {

/// A perspective camera. The matrices are derived on demand, so a moving camera
/// is just changed fields.
struct Camera {
    Vec3 eye{0.0f, 2.8f, 6.2f};
    Vec3 target{0.0f, 0.6f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    float fov_y_radians = Radians(50.0f);
    float near_z = 0.1f;
    float far_z = 200.0f;

    Mat4 View() const { return LookAt(eye, target, up); }
    Mat4 Projection(float aspect) const { return Perspective(fov_y_radians, aspect, near_z, far_z); }
};

/// One directional light.
struct Light {
    /// World space, pointing TOWARDS the light.
    Vec3 direction{0.45f, 0.80f, 0.40f};
    Vec3 colour{0.95f, 0.92f, 0.85f};
};

/// How many lights the shaders process. The shading code is unrolled over this
/// count, so a scene with fewer lights zero-fills the rest, and more than this
/// are ignored rather than being a silent recompile.
inline constexpr std::uint32_t kMaxLights = 4;

/// What a surface is made of: a tint, and optionally a texture to modulate it.
struct Material {
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    /// Blinn-Phong highlight strength. The exponent is fixed in the shader, so
    /// this is the only knob a material has over the specular lobe.
    float specular = 0.25f;
    /// Index into Scene::textures, or kNoTexture. The albedo.
    std::uint32_t texture = kNoTexture;
    /// Index into Scene::textures, or kNoTexture. A tangent-space normal map;
    /// only meaningful alongside an albedo texture, since that is the shader
    /// variant that samples it.
    std::uint32_t normal_texture = kNoTexture;

    bool Textured() const { return texture != kNoTexture; }
    bool NormalMapped() const { return normal_texture != kNoTexture; }
};

/// A square RGBA8 checkerboard. A pattern rather than a flat colour is the
/// point: a wrong uv, filter or wrap shows up as a wrong pattern, where a solid
/// colour looks the same whatever the sampler did.
inline Texture MakeCheckerTexture(std::uint32_t size, std::uint32_t cells) {
    Texture texture;
    texture.width = size;
    texture.height = size;
    texture.rgba.resize(static_cast<std::size_t>(size) * size * 4);
    std::uint32_t cell = cells == 0 ? 1 : size / cells;
    if (cell == 0) {
        cell = 1;
    }
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const bool light = ((x / cell) + (y / cell)) % 2 == 0;
            const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
            texture.rgba[at + 0] = light ? 112 : 54;
            texture.rgba[at + 1] = light ? 120 : 61;
            texture.rgba[at + 2] = light ? 107 : 56;
            texture.rgba[at + 3] = 255;
        }
    }
    return texture;
}

/// A tangent-space normal map of a sine ripple, so a flat surface shades as
/// though it were corrugated. R is the tangent axis and G the bitangent, which is
/// the convention the baked vertex tangent frames assume.
inline Texture MakeRippleNormalTexture(std::uint32_t size, std::uint32_t cells) {
    Texture texture;
    texture.width = size;
    texture.height = size;
    texture.rgba.resize(static_cast<std::size_t>(size) * size * 4);
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kBump = 0.6f;
    const float frequency = cells == 0 ? 1.0f : static_cast<float>(cells);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const float phase_u = kTwoPi * frequency * u;
            const float phase_v = kTwoPi * frequency * v;
            // Analytic slope of sin(phase_u) * sin(phase_v), scaled down so the
            // perturbation stays a bump and does not fold the surface over.
            const float slope_u = -kBump * std::cos(phase_u) * std::sin(phase_v);
            const float slope_v = -kBump * std::sin(phase_u) * std::cos(phase_v);
            const float inverse_length =
                1.0f / std::sqrt(slope_u * slope_u + slope_v * slope_v + 1.0f);
            const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
            texture.rgba[at + 0] = static_cast<std::uint8_t>(
                std::min(255.0f, std::max(0.0f, (slope_u * inverse_length * 0.5f + 0.5f) * 255.0f)));
            texture.rgba[at + 1] = static_cast<std::uint8_t>(
                std::min(255.0f, std::max(0.0f, (slope_v * inverse_length * 0.5f + 0.5f) * 255.0f)));
            texture.rgba[at + 2] = static_cast<std::uint8_t>(
                std::min(255.0f, std::max(0.0f, (inverse_length * 0.5f + 0.5f) * 255.0f)));
            texture.rgba[at + 3] = 255;
        }
    }
    return texture;
}

/// A node that draws nothing: it exists only to carry a transform its children
/// compose with.
inline constexpr std::uint32_t kNoMesh = 0xFFFF'FFFFu;
inline constexpr std::int32_t kNoParent = -1;

/// One node in the scene's transform tree. `local` is relative to `parent`; the
/// renderer composes the path to get the world matrix.
struct Node {
    /// Index into Scene::meshes, or kNoMesh for a pure transform group.
    std::uint32_t mesh = kNoMesh;
    /// Index into Scene::materials.
    std::uint32_t material = 0;
    Mat4 local{};
    /// Index into Scene::nodes, or kNoParent for a root. A parent must appear
    /// before its children in the list, so one forward pass composes every world
    /// matrix.
    std::int32_t parent = kNoParent;

    bool Drawable() const { return mesh != kNoMesh; }
};

/// A tone and a noise burst, so a scene can be heard without shipping a file. The
/// texture makers above play the same part: something a scene can be made out of.
///
/// Both are written to every channel identically -- what makes a sound directional is
/// the emitter it plays from, not the data.
inline audio::Clip MakeTone(float frequency, float seconds,
                            std::uint32_t rate = audio::kSampleRate) {
    audio::Clip clip;
    clip.rate = rate == 0 ? audio::kSampleRate : rate;
    constexpr float kTwoPi = 6.28318530717958647692f;
    const auto frames = static_cast<std::size_t>(static_cast<double>(seconds) * clip.rate);
    clip.frames.reserve(frames);
    for (std::size_t index = 0; index < frames; ++index) {
        const float phase =
            kTwoPi * frequency * static_cast<float>(index) / static_cast<float>(clip.rate);
        const float value = std::sin(phase) * 0.5f;
        clip.frames.push_back(audio::Frame{value, value});
    }
    return clip;
}

/// White noise from a plain linear congruential generator: the same seed makes the same
/// samples on every machine, which is what lets a mix be compared byte for byte.
inline audio::Clip MakeNoise(float seconds, std::uint32_t seed = 1,
                             std::uint32_t rate = audio::kSampleRate) {
    audio::Clip clip;
    clip.rate = rate == 0 ? audio::kSampleRate : rate;
    const auto frames = static_cast<std::size_t>(static_cast<double>(seconds) * clip.rate);
    clip.frames.reserve(frames);
    std::uint32_t state = seed == 0 ? 1u : seed;
    for (std::size_t index = 0; index < frames; ++index) {
        state = state * 1664525u + 1013904223u;
        // The top bits of an LCG are the ones it mixes best: [0, 2) shifted to [-1, 1).
        const float value =
            static_cast<float>(state >> 8) * (1.0f / 8388608.0f) - 1.0f;
        clip.frames.push_back(audio::Frame{value * 0.5f, value * 0.5f});
    }
    return clip;
}

inline constexpr std::int32_t kNoSound = -1;

/// One sound source. It is a node in the same transform tree a drawable is, so a sound
/// attached to a moving object is parented to that object's node. The listener is the
/// scene's camera, so an emitter on the camera is a sound inside the player's head.
struct Emitter {
    /// Index into Scene::sounds, or kNoSound for a silent placeholder.
    std::int32_t sound = kNoSound;
    Mat4 local{};
    /// Index into Scene::nodes, or kNoParent. Same rule as a node: a parent comes
    /// before its children in the list.
    std::int32_t parent = kNoParent;
    /// Volume at the reference distance.
    float gain = 1.0f;
    /// Full volume inside this many world units and silence at `max_distance`, with a
    /// straight line between -- so a caller can work out what moving an emitter does
    /// without reading the mixer.
    float reference_distance = 1.0f;
    float max_distance = 20.0f;
    /// Whether the sound restarts when it ends.
    bool loop = false;
};

struct Scene {
    Camera camera;    /// Directional lights, in shading order. Only the first kMaxLights are used.
    std::vector<Light> lights;
    /// Flat ambient, added to every fragment whatever the material.
    Vec3 ambient{0.26f, 0.28f, 0.34f};
    /// Edge length of the shadow map, or 0 for no shadows. Shadows are cast by
    /// the first light, so a scene with no lights has none either way.
    std::uint32_t shadow_map_size = 0;
    /// What the colour target is cleared to before anything is drawn.
    std::array<float, 4> background{0.05f, 0.06f, 0.10f, 1.0f};
    std::vector<Mesh> meshes;
    std::vector<Texture> textures;
    std::vector<Material> materials;

    /// The transform tree, parents before children.
    /// Clip space = projection * view * world.
    std::vector<Node> nodes;

    /// What is playing, and where from. The sound renderer walks the same node tree
    /// the renderer does, so an emitter's parent is a node and its world position is
    /// wherever that node puts it.
    std::vector<audio::Clip> sounds;
    std::vector<Emitter> emitters;
    /// Applied to the whole mix, on top of every emitter's own gain.
    float master_gain = 1.0f;

    /// Append a node parented to `parent` (or a root) and return its index.
    std::int32_t AddNode(std::uint32_t mesh, std::uint32_t material, const Mat4& local,
                         std::int32_t parent = kNoParent) {
        Node node;
        node.mesh = mesh;
        node.material = material;
        node.local = local;
        node.parent = parent;
        nodes.push_back(node);
        return static_cast<std::int32_t>(nodes.size() - 1);
    }

    /// Append an emitter parented to `parent` (or a root) and return its index.
    std::int32_t AddEmitter(std::int32_t sound, const Mat4& local, std::int32_t parent = kNoParent,
                            float gain = 1.0f) {
        Emitter emitter;
        emitter.sound = sound;
        emitter.local = local;
        emitter.parent = parent;
        emitter.gain = gain;
        emitters.push_back(emitter);
        return static_cast<std::int32_t>(emitters.size() - 1);
    }
};

}  // namespace zlong::engine
