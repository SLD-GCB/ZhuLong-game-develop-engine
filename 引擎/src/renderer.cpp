#include "zlong/engine/renderer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <utility>

#include "zlong/gpu/engine/maxwell3d.h"
#include "zlong/gpu/render/state.h"
#include "zlong/gpu/shader/maxwell.h"
#include "zlong/gpu/shader/maxwell_asm.h"
#include "zlong/gpu/surface/format.h"

namespace zlong::engine {

using zlong::gpu::GpuVAddr;

namespace {

using zlong::gpu::engine::MethodOpcode;
using zlong::gpu::GuestPa;
using zlong::gpu::shader::DecodeShader;
using zlong::gpu::shader::MaxwellProgram;
using zlong::gpu::shader::Module;
using zlong::gpu::shader::MufuFunction;
using zlong::gpu::shader::Stage;
using zlong::gpu::shader::Swizzle;

/// The constant block the vertex and fragment shaders read. The byte offsets are
/// written against this layout, and the static_assert is what keeps the two from
/// drifting apart.
///
/// All three matrices are the transposed ones: every shader that transforms with one
/// accumulates a *column* at a time, because a register is one operand and `ld`
/// fetches sixteen contiguous bytes, so the columns are what has to be adjacent.
/// `model`'s fourth column additionally has its homogeneous one replaced by a zero;
/// the reason is in build_constants.
struct InstanceConstants {
    float mvp[16];                 //   0 - transposed
    float model[16];               //  64 - transposed, fourth column's w replaced by 0
    float tint[4];                 // 128
    float specular[4];             // 144 - strength in lane 0
    float view_position[4];        // 160 - eye in world space, lane 3 zero
    float ambient[4];              // 176 - lane 3 one so alpha survives the sum
    float shadow_matrix[16];       // 192 - transposed light-space view-projection
    float shadow_params[4];        // 256 - x bias, y strength
    float lights[kMaxLights][8];   // 272 - direction (16 bytes), colour (16 bytes)
};

constexpr std::uint32_t kMvpOffset = 0;
constexpr std::uint32_t kModelOffset = 64;
constexpr std::uint32_t kTintOffset = 128;
constexpr std::uint32_t kSpecularOffset = 144;
constexpr std::uint32_t kViewPositionOffset = 160;
constexpr std::uint32_t kAmbientOffset = 176;
constexpr std::uint32_t kShadowMatrixOffset = 192;
constexpr std::uint32_t kShadowParamsOffset = 256;
constexpr std::uint32_t kLightsOffset = 272;
constexpr std::uint32_t kLightStride = 32;
static_assert(sizeof(InstanceConstants) == kLightsOffset + kMaxLights * kLightStride,
              "the shader offsets assume this layout");

/// The one constant buffer slot the renderer binds, so it is the bank every load
/// from it names.
constexpr std::uint32_t kConstantBank = 0;

/// The register a literal zero is written as. Maxwell's RZ reads zero and a write to
/// it goes nowhere, and both consumers of the IR agree -- see `Value` in
/// shader/ir.h -- so the shaders below can name it instead of keeping a zeroed
/// register alive.
constexpr std::uint32_t kRz = zlong::gpu::shader::kFormat.zero_register;

/// The texture slot the shadow map is bound on, after albedo (0) and a normal map
/// (1).
constexpr std::uint32_t kShadowTextureSlot = 2;

/// The vertex shader output the shadow pass's fragment shader interpolates: the
/// clip position of whichever projection that pass was given, which for the shadow
/// pass is the light's.
constexpr std::uint32_t kLightClipSlot = 6;

/// Shader slots, in the order the renderer installs them.
constexpr std::size_t kUntexturedShader = 0;
constexpr std::size_t kTexturedShader = 1;
constexpr std::size_t kNormalMappedShader = 2;
constexpr std::size_t kUntexturedShadowShader = 3;
constexpr std::size_t kTexturedShadowShader = 4;
constexpr std::size_t kNormalMappedShadowShader = 5;
constexpr std::size_t kShadowDepthShader = 6;


/// Position, normal, uv and the tangent frame in; clip position, world normal, uv
/// and world position, world tangent and world bitangent out -- as Maxwell words.
///
/// The transform is why this is not a short shader. The ALU this decoder implements
/// is four-lane lane-wise with no horizontal add, so a dot product is one broadcast
/// (`fswzadd`) and one accumulate (`ffma`) per term, and `ld` fetches sixteen
/// contiguous bytes, so the accumulation runs over *columns* of the matrix -- which
/// is why `mvp` and `model` are stored transposed. All four columns of each, because
/// the fourth is what carries the translation.
///
/// The world vectors' fourth lane comes out zero, which matters: the fragment stage
/// sums all four lanes of a dot product, and a stray one there would skew every
/// length. Two things make it zero -- the model matrix's last row is affine, so the
/// first three terms of that lane vanish, and the stored fourth column has its
/// homogeneous one replaced by a zero (see build_constants), so the last term does
/// too. The clip position keeps its one, because its fourth lane *is* w.
///
/// The clip position is written twice: once as output 0, which the rasteriser takes
/// as the vertex's position, and once as output 6, which is an ordinary varying.
/// The second copy is what the shadow pass reads -- that pass is given the *light's*
/// view-projection as the draw's, so its clip position is already the fragment's
/// light-space position.
std::optional<Module> SceneVertexShader(std::string& error) {
    // Registers.
    constexpr std::uint32_t kPosition = 0;   // attribute slot 0
    constexpr std::uint32_t kNormal = 1;     // attribute slot 1
    constexpr std::uint32_t kUv = 2;         // attribute slot 2
    constexpr std::uint32_t kTangent = 3;    // attribute slot 3
    constexpr std::uint32_t kBitangent = 4;  // attribute slot 4
    /// The position's four components, one per lane: shared by the clip position
    /// and the world position, which transform the same attribute.
    constexpr std::uint32_t kPositionLane[4] = {5, 6, 7, 8};
    constexpr std::uint32_t kColumn = 9;  // one mvp column at a time
    constexpr std::uint32_t kClip = 10;
    /// Broadcasts of whichever attribute is being transformed. The four are reused
    /// for the normal, the tangent and the bitangent in turn.
    constexpr std::uint32_t kComponent[4] = {11, 12, 13, 14};
    constexpr std::uint32_t kWorldNormal = 15;
    constexpr std::uint32_t kWorldPosition = 16;
    constexpr std::uint32_t kWorldTangent = 17;
    constexpr std::uint32_t kWorldBitangent = 18;
    /// The model matrix's four columns, loaded once and shared by all four world
    /// vectors.
    constexpr std::uint32_t kModel[4] = {19, 20, 21, 22};
    constexpr std::uint32_t kRegisters = 23;

    MaxwellProgram program;
    program.LdAttribute(kPosition, 0)
        .LdAttribute(kNormal, 1)
        .LdAttribute(kUv, 2)
        .LdAttribute(kTangent, 3)
        .LdAttribute(kBitangent, 4)
        .Broadcast(kPositionLane[0], kPosition, Swizzle::X)
        .Broadcast(kPositionLane[1], kPosition, Swizzle::Y)
        .Broadcast(kPositionLane[2], kPosition, Swizzle::Z)
        .Broadcast(kPositionLane[3], kPosition, Swizzle::W);

    // clip = mvp * position, one column per step. The fourth column is why this is
    // four steps and not three: for a perspective it carries the -z that becomes w,
    // and for the light's orthographic it carries the one.
    program.LdConst(kColumn, kConstantBank, kMvpOffset + 0 * 16)
        .FMulReg(kClip, kPositionLane[0], kColumn)
        .LdConst(kColumn, kConstantBank, kMvpOffset + 1 * 16)
        .FFmaReg(kClip, kPositionLane[1], kColumn, kClip)
        .LdConst(kColumn, kConstantBank, kMvpOffset + 2 * 16)
        .FFmaReg(kClip, kPositionLane[2], kColumn, kClip)
        .LdConst(kColumn, kConstantBank, kMvpOffset + 3 * 16)
        .FFmaReg(kClip, kPositionLane[3], kColumn, kClip);

    for (std::uint32_t column = 0; column < 4; ++column) {
        program.LdConst(kModel[column], kConstantBank, kModelOffset + column * 16);
    }

    // The world position reuses the position's broadcasts.
    program.FMulReg(kWorldPosition, kPositionLane[0], kModel[0])
        .FFmaReg(kWorldPosition, kPositionLane[1], kModel[1], kWorldPosition)
        .FFmaReg(kWorldPosition, kPositionLane[2], kModel[2], kWorldPosition)
        .FFmaReg(kWorldPosition, kPositionLane[3], kModel[3], kWorldPosition);

    // The normal, the tangent and the bitangent are each the same four-column
    // reduction, so they share one sequence.
    const auto to_world = [&](std::uint32_t destination, std::uint32_t source) {
        program.Broadcast(kComponent[0], source, Swizzle::X)
            .Broadcast(kComponent[1], source, Swizzle::Y)
            .Broadcast(kComponent[2], source, Swizzle::Z)
            .Broadcast(kComponent[3], source, Swizzle::W)
            .FMulReg(destination, kComponent[0], kModel[0])
            .FFmaReg(destination, kComponent[1], kModel[1], destination)
            .FFmaReg(destination, kComponent[2], kModel[2], destination)
            .FFmaReg(destination, kComponent[3], kModel[3], destination);
    };
    to_world(kWorldNormal, kNormal);
    to_world(kWorldTangent, kTangent);
    to_world(kWorldBitangent, kBitangent);

    program.StoreAttribute(0, kClip)
        .StoreAttribute(1, kWorldNormal)
        .StoreAttribute(2, kUv)
        .StoreAttribute(3, kWorldPosition)
        .StoreAttribute(4, kWorldTangent)
        .StoreAttribute(5, kWorldBitangent)
        .StoreAttribute(6, kClip)
        .Exit();

    return DecodeShader(program.Build(kRegisters), Stage::Vertex, error);
}

/// The four broadcasts a lane-wise reduction needs: with no horizontal add to lean
/// on, a dot product is built out of scalars that are already in every lane.
void EmitBroadcasts(MaxwellProgram& program, const std::uint32_t (&targets)[4],
                    std::uint32_t source) {
    program.Broadcast(targets[0], source, Swizzle::X)
        .Broadcast(targets[1], source, Swizzle::Y)
        .Broadcast(targets[2], source, Swizzle::Z)
        .Broadcast(targets[3], source, Swizzle::W);
}

/// result = sum over j of first[j] * second[j], both already broadcast. What
/// `Op::FDot` does in one instruction costs four here -- and it lands in *every*
/// lane rather than in lane 0, which is why nothing has to splat it back out.
///
/// The arithmetic matches: the IR's `FFma` is lowered as a multiply and then an add
/// by both of its consumers, not as a fused multiply-add, so the product is rounded
/// before the sum exactly here too.
void EmitDot(MaxwellProgram& program, std::uint32_t result, const std::uint32_t (&first)[4],
             const std::uint32_t (&second)[4]) {
    program.FMulReg(result, first[0], second[0])
        .FFmaReg(result, first[1], second[1], result)
        .FFmaReg(result, first[2], second[2], result)
        .FFmaReg(result, first[3], second[3], result);
}

/// The scene's fragment shader, in the six shapes the renderer needs: tinted or
/// textured, optionally with a normal map, optionally with the shadow term -- as
/// Maxwell words. One builder rather than six hand-written modules, because the
/// shading is the same and the differences are three flags.
///
/// Blinn-Phong over the scene's lights, exactly as the IR version computed it:
///   light = sum_i max(dot(N, L_i), 0) * colour_i
///   spec  = sum_i max(dot(N, H_i), 0)^16 * colour_i
/// with ambient added afterwards so a shadow dims the direct light and not the
/// ambient. The exponent 16 is four squarings.
///
/// Registers: 0 normal, 1 uv, 2 world position, 3 tangent, 4 bitangent, 5 albedo
/// sample, 6 normal-map sample, 7 tangent-space normal, 8 light, 9 spec, 10 view,
/// 11 dot, 12 spread, 13 dir, 14 colour, 15 half, 16 chain, 18 scratch, 19 tint,
/// 20 out, 21 shaded normal, 22 shadow clip, 23 shadow uv, 24 shadow sample,
/// 25 shadow factor, 26 one, 27..30 the first operand's four broadcasts, 31..34 the
/// second's. There is no zero register among them: a literal zero is RZ, which both
/// consumers read as zero.
std::optional<Module> FragmentShader(bool textured, bool normal_mapped, bool shadowed,
                                     std::string& error) {
    constexpr std::uint32_t kNormal = 0;
    constexpr std::uint32_t kUv = 1;
    constexpr std::uint32_t kWorld = 2;
    constexpr std::uint32_t kTangent = 3;
    constexpr std::uint32_t kBitangent = 4;
    constexpr std::uint32_t kAlbedo = 5;
    constexpr std::uint32_t kNormalSample = 6;
    constexpr std::uint32_t kTangentNormal = 7;
    constexpr std::uint32_t kLight = 8;
    constexpr std::uint32_t kSpec = 9;
    constexpr std::uint32_t kView = 10;
    constexpr std::uint32_t kDot = 11;
    constexpr std::uint32_t kSpread = 12;
    constexpr std::uint32_t kDir = 13;
    constexpr std::uint32_t kColour = 14;
    constexpr std::uint32_t kHalf = 15;
    constexpr std::uint32_t kChain = 16;
    constexpr std::uint32_t kScratch = 18;
    constexpr std::uint32_t kTint = 19;
    constexpr std::uint32_t kOut = 20;
    constexpr std::uint32_t kShaded = 21;
    constexpr std::uint32_t kShadowClip = 22;
    constexpr std::uint32_t kShadowUv = 23;
    constexpr std::uint32_t kShadowSample = 24;
    constexpr std::uint32_t kShadowFactor = 25;
    constexpr std::uint32_t kOne = 26;
    constexpr std::uint32_t kFirst[4] = {27, 28, 29, 30};
    constexpr std::uint32_t kSecond[4] = {31, 32, 33, 34};
    constexpr std::uint32_t kRegisters = 35;

    MaxwellProgram program;

    // Interpolate the inputs this variant needs, by the slot the vertex shader wrote
    // them to. The decoder declares exactly the slots the code names, so the two
    // ends meet on location.
    program.Ipa(kNormal, 1);
    if (textured) {
        program.Ipa(kUv, 2);
    }
    program.Ipa(kWorld, 3);
    if (normal_mapped) {
        program.Ipa(kTangent, 4).Ipa(kBitangent, 5);
    }

    // The samples, and the shading normal. A normal map arrives in tangent space and
    // is carried back out with the interpolated frame, which is orthogonal and
    // equally scaled because the model matrix is a uniform scale -- so the weighted
    // sum below is already the right direction and the lighting normalizes it.
    const std::uint32_t shaded = normal_mapped ? kShaded : kNormal;
    if (normal_mapped) {
        program.Tex2D(kAlbedo, kUv, 0);
        program.Tex2D(kNormalSample, kUv, 1);
        program.Mov32I(kScratch, 2.0f);
        program.FMulReg(kTangentNormal, kNormalSample, kScratch);
        program.Mov32I(kScratch, 1.0f);
        program.FSubReg(kTangentNormal, kTangentNormal, kScratch);
        program.Broadcast(kSpread, kTangentNormal, Swizzle::X);
        program.FMulReg(kShaded, kTangent, kSpread);
        program.Broadcast(kSpread, kTangentNormal, Swizzle::Y);
        program.FFmaReg(kShaded, kBitangent, kSpread, kShaded);
        program.Broadcast(kSpread, kTangentNormal, Swizzle::Z);
        program.FFmaReg(kShaded, kNormal, kSpread, kShaded);
    } else if (textured) {
        program.Tex2D(kAlbedo, kUv, 0);
    }

    // The shadow is computed first, because it scales the direct light and the
    // lighting below is what builds that light.
    if (shadowed) {
        // clip = shadow matrix * (world, 1), accumulated one column at a time. The
        // fourth column is the homogeneous one's contribution, so it is added rather
        // than multiplied.
        EmitBroadcasts(program, kFirst, kWorld);
        program.LdConst(kScratch, kConstantBank, kShadowMatrixOffset + 0 * 16)
            .FMulReg(kShadowClip, kFirst[0], kScratch)
            .LdConst(kScratch, kConstantBank, kShadowMatrixOffset + 1 * 16)
            .FFmaReg(kShadowClip, kFirst[1], kScratch, kShadowClip)
            .LdConst(kScratch, kConstantBank, kShadowMatrixOffset + 2 * 16)
            .FFmaReg(kShadowClip, kFirst[2], kScratch, kShadowClip)
            .LdConst(kScratch, kConstantBank, kShadowMatrixOffset + 3 * 16)
            .FAddReg(kShadowClip, kShadowClip, kScratch);

        // Perspective divide, then clip space to [0, 1]. That leaves the sample
        // coordinate in lanes 0 and 1 and the fragment's own depth in lane 2, which
        // is the pair the comparison below wants side by side.
        program.Broadcast(kScratch, kShadowClip, Swizzle::W)
            .Mufu(kScratch, kScratch, MufuFunction::Rcp)
            .FMulReg(kShadowClip, kShadowClip, kScratch)
            .Mov32I(kOne, 0.5f)
            .FMulReg(kShadowUv, kShadowClip, kOne)
            .FAddReg(kShadowUv, kShadowUv, kOne)
            .Tex2D(kShadowSample, kShadowUv, kShadowTextureSlot)
            .Broadcast(kShadowSample, kShadowSample, Swizzle::X)
            .LdConst(kScratch, kConstantBank, kShadowParamsOffset)
            .Broadcast(kScratch, kScratch, Swizzle::X)  // bias
            .FAddReg(kShadowSample, kShadowSample, kScratch)
            .Broadcast(kShadowFactor, kShadowUv, Swizzle::Z)  // the fragment's own depth
            .FSubReg(kShadowFactor, kShadowSample, kShadowFactor);

        // lit = clamp(ceil(stored + bias - depth), 0, 1). There is no select and no
        // predicate to build a step from, and the family has no ceil, so ceil is the
        // floor of the negation undone -- the same three steps the IR used.
        program.Negate(kShadowFactor, kShadowFactor)
            .Floor(kShadowFactor, kShadowFactor)
            .Negate(kShadowFactor, kShadowFactor)
            .Mov32I(kOne, 1.0f)
            .FMinReg(kShadowFactor, kShadowFactor, kOne)
            .FMaxReg(kShadowFactor, kShadowFactor, kRz)
            .LdConst(kScratch, kConstantBank, kShadowParamsOffset)
            .Broadcast(kScratch, kScratch, Swizzle::Y)  // strength
            .FMulReg(kShadowFactor, kShadowFactor, kScratch)
            .FSubReg(kOne, kOne, kScratch)
            .FAddReg(kShadowFactor, kShadowFactor, kOne);
    }

    // n /= |n|. The interpolated normal is not unit -- the model matrix scales it --
    // and a uniform scale of 24 made the ground blow out to white before this.
    EmitBroadcasts(program, kFirst, shaded);
    EmitDot(program, kDot, kFirst, kFirst);
    program.Mufu(kDot, kDot, MufuFunction::Sqrt)
        .Mufu(kDot, kDot, MufuFunction::Rcp)
        .FMulReg(shaded, shaded, kDot);

    // v = normalize(eye - world position). Lane 3 of both is zero, so the fourth
    // term of the reduction contributes nothing.
    program.LdConst(kView, kConstantBank, kViewPositionOffset)
        .FSubReg(kView, kView, kWorld);
    EmitBroadcasts(program, kFirst, kView);
    EmitDot(program, kDot, kFirst, kFirst);
    program.Mufu(kDot, kDot, MufuFunction::Sqrt)
        .Mufu(kDot, kDot, MufuFunction::Rcp)
        .FMulReg(kView, kView, kDot);

    // The accumulated light and highlight start at zero.
    program.Mov32I(kLight, 0.0f).Mov32I(kSpec, 0.0f);

    // The lights are unrolled rather than looped, and an unused slot contributes
    // nothing because its colour is zero.
    EmitBroadcasts(program, kFirst, shaded);  // the normalized normal, this time
    for (std::uint32_t index = 0; index < kMaxLights; ++index) {
        const std::uint32_t base = kLightsOffset + index * kLightStride;
        program.LdConst(kDir, kConstantBank, base)
            .LdConst(kColour, kConstantBank, base + 16);

        // Diffuse: light += max(dot(n, l), 0) * colour.
        EmitBroadcasts(program, kSecond, kDir);
        EmitDot(program, kDot, kFirst, kSecond);
        program.FMaxReg(kSpread, kDot, kRz)
            .FFmaReg(kLight, kSpread, kColour, kLight);

        // Half vector, then the highlight: spec += dot(n, h)^16 * colour.
        program.FAddReg(kHalf, kDir, kView);
        EmitBroadcasts(program, kSecond, kHalf);
        EmitDot(program, kDot, kSecond, kSecond);
        program.Mufu(kDot, kDot, MufuFunction::Sqrt)
            .Mufu(kDot, kDot, MufuFunction::Rcp)
            .FMulReg(kHalf, kHalf, kDot);
        EmitBroadcasts(program, kSecond, kHalf);
        EmitDot(program, kDot, kFirst, kSecond);
        program.FMaxReg(kChain, kDot, kRz);
        for (int squaring = 0; squaring < 4; ++squaring) {
            program.FMulReg(kChain, kChain, kChain);
        }
        program.FFmaReg(kSpec, kChain, kColour, kSpec);
    }

    if (shadowed) {
        program.FMulReg(kLight, kLight, kShadowFactor).FMulReg(kSpec, kSpec, kShadowFactor);
    }

    // Ambient is added after the shadow, so an occluded surface keeps it.
    program.LdConst(kScratch, kConstantBank, kAmbientOffset)
        .FAddReg(kLight, kLight, kScratch);

    program.LdConst(kTint, kConstantBank, kTintOffset);
    if (textured) {
        program.FMulReg(kAlbedo, kAlbedo, kTint);
    }
    program.FMulReg(kOut, textured ? kAlbedo : kTint, kLight);
    program.LdConst(kScratch, kConstantBank, kSpecularOffset)
        .Broadcast(kSpread, kScratch, Swizzle::X)  // the material's strength
        .FMulReg(kSpec, kSpec, kSpread)
        .FAddReg(kOut, kOut, kSpec)
        .StoreAttribute(0, kOut)
        .Exit();

    return DecodeShader(program.Build(kRegisters), Stage::Fragment, error);
}

/// The shadow pass's fragment shader, written as Maxwell instruction words and
/// decoded back into the IR the pipeline runs.
///
/// It does no transform of its own. The shadow pass's vertex shader is handed the
/// light's view-projection, so the clip position it computes *is* the fragment's
/// light-space position; the vertex shader publishes that as its own varying and
/// this reads it back, divides by w and maps it into [0, 1] the way the sampling
/// side reads it. Doing the transform here instead -- which this used to do -- is
/// the same arithmetic with a matrix load and a reduction per pixel.
///
/// The words are written in this file, so a refusal means the engine and the
/// encoder have drifted; there is nothing sensible to fall back to, and the
/// renderer reports the failure instead of drawing with a shader that does nothing.
std::optional<Module> ShadowDepthFragmentShader(std::string& error) {
    // Registers: 0 the light-space clip position, 1 the reciprocal of its w, 2 a
    // half. No instruction names a constant or immediate ALU operand, so there is
    // nothing for the decoder to materialise and these three are the whole file.
    constexpr std::uint32_t kClip = 0;
    constexpr std::uint32_t kInverseW = 1;
    constexpr std::uint32_t kHalf = 2;
    constexpr std::uint32_t kRegisters = 3;

    MaxwellProgram program;
    program.Ipa(kClip, kLightClipSlot)
        .Broadcast(kInverseW, kClip, Swizzle::W)
        .Mufu(kInverseW, kInverseW, MufuFunction::Rcp)
        .FMulReg(kClip, kClip, kInverseW)  // perspective divide
        .Mov32I(kHalf, 0.5f)
        .FFmaReg(kClip, kClip, kHalf, kHalf)   // the clip space [-1, 1] to [0, 1]
        .Broadcast(kClip, kClip, Swizzle::Z)   // lane 2 is the depth; carry it to lane 0
        .StoreAttribute(0, kClip)
        .Exit();

    return DecodeShader(program.Build(kRegisters), Stage::Fragment, error);
}

/// Assembles a shader from Maxwell words and decodes it back into the IR.
///
/// Both builders above write their words in this file, so a refusal means the engine
/// and the encoder have drifted; there is nothing to fall back to, and an empty
/// module is returned only so that the failure can be carried somewhere it can be
/// reported -- `Prepare` and `Render` refuse while `shader_error_` is set, so it is
/// never drawn with. Only the first failure is kept: both come from the same
/// encoder and one explanation is enough.
Module AssembleShader(std::optional<Module> (*build)(std::string&), std::string& error) {
    std::string failure;
    std::optional<Module> module = build(failure);
    if (!failure.empty() && error.empty()) {
        error = std::move(failure);
    }
    return module.value_or(Module{});
}

// ----------------------------------------------------------- command stream --

std::uint32_t Header(MethodOpcode opcode, std::uint32_t address, std::uint32_t count_minus_one) {
    return (static_cast<std::uint32_t>(opcode) << 29) | ((count_minus_one & 0x1FFFu) << 16) |
           (((address / 4) & 0x7FFu) << 2);
}

void Emit(std::vector<std::uint32_t>& words, std::uint32_t address,
          const std::vector<std::uint32_t>& arguments) {
    words.push_back(
        Header(MethodOpcode::Increment, address, static_cast<std::uint32_t>(arguments.size()) - 1));
    words.insert(words.end(), arguments.begin(), arguments.end());
}

std::uint32_t Low(std::uint64_t value) { return static_cast<std::uint32_t>(value); }
std::uint32_t High(std::uint64_t value) { return static_cast<std::uint32_t>(value >> 32); }

std::uint32_t Bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// A rough upper bound on the words one drawable contributes to one pass: bind
/// the mesh, its five attributes, up to three textures, both counts, the constants
/// and the draw. A scene with shadows walks the drawables twice.
constexpr std::size_t kWordsPerInstance = 192;

/// The mesh's local-space bounds, from its vertices. Prepare rejects empty
/// meshes, so `vertices` is never empty here.
Bound BoundOf(const Mesh& mesh) {
    const MeshVertex& first = mesh.vertices.front();
    Bound bound;
    bound.min = Vec3{first.position[0], first.position[1], first.position[2]};
    bound.max = bound.min;
    for (const MeshVertex& vertex : mesh.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            const float value = vertex.position[axis];
            (&bound.min.x)[axis] = std::min((&bound.min.x)[axis], value);
            (&bound.max.x)[axis] = std::max((&bound.max.x)[axis], value);
        }
    }
    return bound;
}

/// The light-space view-projection for a directional light, fitted to the scene's
/// world bounds. The eye sits a scene-radius outside the bounds so the whole box
/// is in front of it, and the orthographic box is square so the map's aspect does
/// not have to match the scene's.
Mat4 ShadowMatrix(const Bound& bounds, const Vec3& direction) {
    const Vec3 centre{(bounds.min.x + bounds.max.x) * 0.5f, (bounds.min.y + bounds.max.y) * 0.5f,
                      (bounds.min.z + bounds.max.z) * 0.5f};
    const Vec3 extent{bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y,
                      bounds.max.z - bounds.min.z};
    const float radius = 0.5f * std::sqrt(extent.x * extent.x + extent.y * extent.y +
                                          extent.z * extent.z);
    const float safe_radius = radius < 0.001f ? 0.001f : radius;

    const Vec3 to_light = Normalize(direction);
    const float distance = safe_radius * 2.0f + 1.0f;
    const Vec3 eye{centre.x - to_light.x * distance, centre.y - to_light.y * distance,
                   centre.z - to_light.z * distance};
    // A light pointing straight down would leave LookAt's up vector degenerate.
    const Vec3 up =
        std::fabs(to_light.y) > 0.99f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Mat4 view = LookAt(eye, centre, up);
    const Mat4 projection =
        Orthographic(safe_radius, safe_radius, distance - safe_radius, distance + safe_radius);
    return projection * view;
}

}  // namespace

Renderer::Renderer(zlong::gpu::Gpu& gpu, zlong::gpu::render::RenderBackend& backend)
    : gpu_(gpu), backend_(backend) {
    // Every stage and every shading variant is assembled from Maxwell instruction
    // words and decoded back. The six variants are one builder with three flags, so
    // they are installed through one lambda; the decoder's hash of the assembled bytes
    // is what tells their pipelines apart, and the flags are what make those bytes
    // differ.
    const auto install = [this](bool textured, bool normal_mapped, bool shadowed, Module& target) {
        std::string failure;
        if (auto module = FragmentShader(textured, normal_mapped, shadowed, failure)) {
            target = std::move(*module);
        } else if (shader_error_.empty()) {
            shader_error_ = std::move(failure);
        }
    };
    install(false, false, false, fragment_shaders_[kUntexturedShader]);
    install(true, false, false, fragment_shaders_[kTexturedShader]);
    install(true, true, false, fragment_shaders_[kNormalMappedShader]);
    install(false, false, true, fragment_shaders_[kUntexturedShadowShader]);
    install(true, false, true, fragment_shaders_[kTexturedShadowShader]);
    install(true, true, true, fragment_shaders_[kNormalMappedShadowShader]);

    vertex_shader_ = AssembleShader(SceneVertexShader, shader_error_);
    shadow_fragment_ = AssembleShader(ShadowDepthFragmentShader, shader_error_);
}

GuestPa Renderer::Allocate(std::size_t bytes, std::size_t alignment) {
    const auto mask = static_cast<std::uint64_t>(alignment) - 1;
    const std::uint64_t aligned = (arena_cursor_ + mask) & ~mask;
    const std::uint64_t end = aligned + bytes;
    // Past the block the kernel reserved. Hand back a null address rather than a real
    // one: address 0 is not guest RAM, so the write that follows fails instead of
    // quietly landing on whatever the next allocation will be.
    if (end - arena_base_ > arena_bytes_) {
        arena_overflow_ = true;
        return 0;
    }
    arena_cursor_ = end;
    return static_cast<GuestPa>(aligned);
}

bool Renderer::Write(GuestPa at, const void* data, std::size_t bytes) {
    return gpu_.memory().GuestWrite(at, data, bytes);
}

bool Renderer::Prepare(const Scene& scene, std::uint32_t width, std::uint32_t height,
                       GuestPa arena_base, std::uint64_t arena_bytes, std::string& error) {
    // A shader this file assembled and the decoder refused: the pipeline cannot be
    // set up, so nothing below is worth doing.
    if (!shader_error_.empty()) {
        error = shader_error_;
        return false;
    }
    width_ = width;
    height_ = height;
    arena_base_ = arena_base;
    arena_bytes_ = arena_bytes;
    arena_cursor_ = arena_base;
    arena_overflow_ = false;
    meshes_.clear();
    mesh_bounds_.clear();
    textures_.clear();
    constant_slots_.clear();
    shadow_slots_.clear();
    drawable_nodes_.clear();
    shadows_ = false;
    shadow_size_ = 0;

    // --- geometry and textures, uploaded once ------------------------------
    for (const Mesh& mesh : scene.meshes) {
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            error = "a scene mesh is empty";
            return false;
        }
        UploadedMesh uploaded;
        uploaded.vertex_bytes = mesh.VertexBytes();
        uploaded.index_bytes = mesh.IndexBytes();
        uploaded.vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
        uploaded.index_count = static_cast<std::uint32_t>(mesh.indices.size());
        uploaded.vertices = Allocate(uploaded.vertex_bytes, 16);
        uploaded.indices = Allocate(uploaded.index_bytes, 16);
        if (!Write(uploaded.vertices, mesh.vertices.data(), uploaded.vertex_bytes) ||
            !Write(uploaded.indices, mesh.indices.data(), uploaded.index_bytes)) {
            error = "the scene's geometry does not fit in memory";
            return false;
        }
        meshes_.push_back(uploaded);
        mesh_bounds_.push_back(BoundOf(mesh));
    }

    for (const Texture& texture : scene.textures) {
        if (!texture.Valid()) {
            error = "a scene texture is empty or its size does not match its texels";
            return false;
        }
        const GuestPa at = Allocate(texture.rgba.size(), 256);
        if (!Write(at, texture.rgba.data(), texture.rgba.size())) {
            error = "the scene's textures do not fit in memory";
            return false;
        }
        textures_.push_back(at);
    }

    // --- the transform tree -------------------------------------------------
    // Every node is checked, drawable or not, because a bad parent link would
    // corrupt the world matrices of everything under it.
    node_count_ = scene.nodes.size();
    for (std::size_t index = 0; index < scene.nodes.size(); ++index) {
        const Node& node = scene.nodes[index];
        if (node.parent != kNoParent &&
            (node.parent < 0 || static_cast<std::size_t>(node.parent) >= index)) {
            error = "a scene node's parent does not come before it";
            return false;
        }
        if (!node.Drawable()) {
            continue;
        }
        if (node.mesh >= meshes_.size()) {
            error = "a scene node names a mesh the scene does not have";
            return false;
        }
        if (node.material >= scene.materials.size()) {
            error = "a scene node names a material the scene does not have";
            return false;
        }
        const Material& material = scene.materials[node.material];
        if (material.Textured() && material.texture >= textures_.size()) {
            error = "a material names a texture the scene does not have";
            return false;
        }
        if (material.NormalMapped() && material.normal_texture >= textures_.size()) {
            error = "a material names a normal map the scene does not have";
            return false;
        }
        drawable_nodes_.push_back(static_cast<std::uint32_t>(index));
        constant_slots_.push_back(Allocate(sizeof(InstanceConstants), 256));
    }

    // --- shadow map --------------------------------------------------------
    // The map is a float COLOUR target rather than a depth texture. The IR can
    // sample a colour target on both backends, and the alternative -- a draw that
    // writes depth and no colour -- needs a blend/write-mask method the command
    // stream does not have.
    shadows_ = scene.shadow_map_size > 0 && !scene.lights.empty();
    if (shadows_) {
        shadow_size_ = scene.shadow_map_size;
        shadow_colour_ =
            Allocate(static_cast<std::size_t>(shadow_size_) * shadow_size_ * 16, 256);
        shadow_depth_ = Allocate(static_cast<std::size_t>(shadow_size_) * shadow_size_ * 4, 256);
        shadow_slots_.resize(drawable_nodes_.size());
        for (GuestPa& slot : shadow_slots_) {
            slot = Allocate(sizeof(InstanceConstants), 256);
        }
    }

    // --- targets -----------------------------------------------------------
    colour_target_ = Allocate(static_cast<std::size_t>(width) * height * 4, 256);
    depth_target_ = Allocate(static_cast<std::size_t>(width) * height * 4, 256);
    stream_ = Allocate((drawable_nodes_.size() + 1) * kWordsPerInstance * sizeof(std::uint32_t),
                       64);
    stream_capacity_ = (drawable_nodes_.size() + 1) * kWordsPerInstance;

    // Every slot has been asked for; if any of them did not fit, say so before anything
    // is mapped. A slot the constant-write path fills later never went through Write,
    // so the overflow flag is the only thing that would have caught it.
    if (arena_overflow_) {
        error = "the renderer's arena is too small for this scene";
        return false;
    }

    // The arena is the GPU's memory, so name it in the GPU's own address space. One
    // mapping covers the whole arena, and every address a stream carries is looked up
    // in it -- the decoder resolves each one and refuses a stream that names memory the
    // device was never told about.
    gpu_.address_space().Map(arena_base, arena_cursor_ - arena_base);

    gpu_.SetShader(kUntexturedShader, &vertex_shader_, &fragment_shaders_[kUntexturedShader]);
    gpu_.SetShader(kTexturedShader, &vertex_shader_, &fragment_shaders_[kTexturedShader]);
    gpu_.SetShader(kNormalMappedShader, &vertex_shader_, &fragment_shaders_[kNormalMappedShader]);
    gpu_.SetShader(kUntexturedShadowShader, &vertex_shader_,
                   &fragment_shaders_[kUntexturedShadowShader]);
    gpu_.SetShader(kTexturedShadowShader, &vertex_shader_,
                   &fragment_shaders_[kTexturedShadowShader]);
    gpu_.SetShader(kNormalMappedShadowShader, &vertex_shader_,
                   &fragment_shaders_[kNormalMappedShadowShader]);
    gpu_.SetShader(kShadowDepthShader, &vertex_shader_, &shadow_fragment_);
    error.clear();
    return true;
}

bool Renderer::Render(const Scene& scene, std::size_t& draws, std::string& error) {
    if (!shader_error_.empty()) {
        error = shader_error_;
        return false;
    }
    if (scene.nodes.size() != node_count_ || drawable_nodes_.size() != constant_slots_.size()) {
        error = "the scene changed shape since Prepare: re-upload it";
        return false;
    }

    // Phase timing. Cheap enough to leave on: the breakdown is what decides where the
    // next change goes, and a frame is milliseconds.
    profile_ = FrameProfile{};
    const auto probe = [] { return std::chrono::steady_clock::now(); };
    const auto since = [](auto started) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         started)
            .count();
    };
    const auto prepare_started = probe();

    // World matrices, one forward pass: parents always precede their children.
    std::vector<Mat4> world(scene.nodes.size());
    for (std::size_t index = 0; index < scene.nodes.size(); ++index) {
        const Node& node = scene.nodes[index];
        world[index] =
            node.parent == kNoParent ? node.local : world[node.parent] * node.local;
    }

    // --- clear, from the host ----------------------------------------------
    // Not through the engine's clear: that runs per draw, so a scene of many
    // objects would wipe itself after the first one.
    const auto to_byte = [](float value) {
        const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
    };
    std::vector<std::uint8_t> background(static_cast<std::size_t>(width_) * height_ * 4);
    for (std::size_t at = 0; at < background.size(); at += 4) {
        background[at + 0] = to_byte(scene.background[0]);
        background[at + 1] = to_byte(scene.background[1]);
        background[at + 2] = to_byte(scene.background[2]);
        background[at + 3] = to_byte(scene.background[3]);
    }
    if (!Write(colour_target_, background.data(), background.size())) {
        error = "the colour target could not be cleared";
        return false;
    }
    std::vector<float> farthest(static_cast<std::size_t>(width_) * height_, 1.0f);
    if (!Write(depth_target_, farthest.data(), farthest.size() * sizeof(float))) {
        error = "the depth target could not be cleared";
        return false;
    }

    // --- per-drawable constants --------------------------------------------
    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const Mat4 view = scene.camera.View();
    const Mat4 projection = scene.camera.Projection(aspect);
    const std::size_t light_count = std::min<std::size_t>(scene.lights.size(), kMaxLights);

    // --- the shadow map, fitted to the scene from the first light -----------
    Bound scene_bounds{};
    bool has_bounds = false;
    for (std::size_t d = 0; d < drawable_nodes_.size(); ++d) {
        const Node& node = scene.nodes[drawable_nodes_[d]];
        const Bound bound = TransformBound(world[drawable_nodes_[d]], mesh_bounds_[node.mesh]);
        if (!has_bounds) {
            scene_bounds = bound;
            has_bounds = true;
            continue;
        }
        scene_bounds.min.x = std::min(scene_bounds.min.x, bound.min.x);
        scene_bounds.min.y = std::min(scene_bounds.min.y, bound.min.y);
        scene_bounds.min.z = std::min(scene_bounds.min.z, bound.min.z);
        scene_bounds.max.x = std::max(scene_bounds.max.x, bound.max.x);
        scene_bounds.max.y = std::max(scene_bounds.max.y, bound.max.y);
        scene_bounds.max.z = std::max(scene_bounds.max.z, bound.max.z);
    }
    if (shadows_ && has_bounds) {
        shadow_matrix_ = ShadowMatrix(scene_bounds, scene.lights[0].direction);
    }

    profile_.prepare_ms = since(prepare_started);

    // --- visibility ---------------------------------------------------------
    // A drawable whose world bound misses the frustum is not submitted to the main
    // pass at all: no constants written for it, no commands emitted for it.
    const Frustum frustum = FrustumFromViewProjection(projection * view);
    std::vector<std::size_t> visible;
    visible.reserve(drawable_nodes_.size());
    for (std::size_t d = 0; d < drawable_nodes_.size(); ++d) {
        const Node& node = scene.nodes[drawable_nodes_[d]];
        const Bound bound = TransformBound(world[drawable_nodes_[d]], mesh_bounds_[node.mesh]);
        if (Intersects(frustum, bound)) {
            visible.push_back(d);
        }
    }

    // Depth is in [0, 1] across the light's frustum, so the bias is in those
    // units. Front-face culling in the shadow pass is what does most of the work;
    // this only has to cover the depth quantisation.
    profile_.prepare_ms = since(prepare_started);

    // A bias in the shadow map's own depth. What it has to cover is the depth a sloped surface
    // changes across one texel: `tan(angle to the light) / map resolution` in these same units, so
    // about 0.0013 for a surface at fifty degrees with a 1024 map. The old 0.0015 sat exactly on
    // that line: flat things -- a board, a wall, a floor -- were fine, and the first large sloped
    // surface put a stripe of self-shadowing across itself. Doubling it costs a twentieth of a
    // unit of peter-panning and takes the whole class of surface off the edge.
    constexpr float kShadowBias = 0.0015f;
    constexpr float kShadowStrength = 0.7f;

    const auto constants_started = probe();
    const auto build_constants = [&](std::size_t d, const Mat4& eye_space) {
        const Node& node = scene.nodes[drawable_nodes_[d]];
        const Mat4& matrix = world[drawable_nodes_[d]];
        InstanceConstants constants{};
        // All three matrices are stored by column, because the shaders that transform
        // with them accumulate one column at a time: `ld` fetches sixteen contiguous
        // bytes, and a four-lane lane-wise ALU has no horizontal add to reduce a row
        // with. The model matrix's fourth column then loses its homogeneous one -- the
        // vertex shader accumulates that column for the translation's sake, and the
        // world vectors' fourth lane has to stay zero.
        const MvpBytes mvp = ToBytesByColumn(eye_space * matrix);
        MvpBytes model = ToBytesByColumn(matrix);
        model.rows[3 * 4 + 3] = 0.0f;
        const MvpBytes shadow = ToBytesByColumn(shadow_matrix_);
        std::memcpy(constants.mvp, mvp.rows, sizeof(constants.mvp));
        std::memcpy(constants.model, model.rows, sizeof(constants.model));
        std::memcpy(constants.shadow_matrix, shadow.rows, sizeof(constants.shadow_matrix));

        const Material& material = scene.materials[node.material];
        std::memcpy(constants.tint, material.tint.data(), sizeof(constants.tint));
        constants.specular[0] = material.specular;
        constants.view_position[0] = scene.camera.eye.x;
        constants.view_position[1] = scene.camera.eye.y;
        constants.view_position[2] = scene.camera.eye.z;
        constants.ambient[0] = scene.ambient.x;
        constants.ambient[1] = scene.ambient.y;
        constants.ambient[2] = scene.ambient.z;
        constants.ambient[3] = 1.0f;  // so alpha survives the light sum

        constants.shadow_params[0] = kShadowBias;
        constants.shadow_params[1] = kShadowStrength;

        // Unused light slots stay zero, and a zero colour contributes nothing, so
        // the shader unrolls over every slot without needing a count.
        for (std::size_t index = 0; index < light_count; ++index) {
            const Vec3 direction = Normalize(scene.lights[index].direction);
            constants.lights[index][0] = direction.x;
            constants.lights[index][1] = direction.y;
            constants.lights[index][2] = direction.z;
            constants.lights[index][4] = scene.lights[index].colour.x;
            constants.lights[index][5] = scene.lights[index].colour.y;
            constants.lights[index][6] = scene.lights[index].colour.z;
        }
        return constants;
    };

    for (std::size_t d : visible) {
        const InstanceConstants constants = build_constants(d, projection * view);
        if (!Write(constant_slots_[d], &constants, sizeof(constants))) {
            error = "the per-drawable constants could not be written";
            return false;
        }
    }
    if (shadows_) {
        // The shadow pass sees every drawable, not just the visible ones: what is
        // off screen can still cast into the frame.
        for (std::size_t d = 0; d < drawable_nodes_.size(); ++d) {
            const InstanceConstants constants = build_constants(d, shadow_matrix_);
            if (!Write(shadow_slots_[d], &constants, sizeof(constants))) {
                error = "the shadow constants could not be written";
                return false;
            }
        }
    }

    if (shadows_) {
        const std::size_t texels = static_cast<std::size_t>(shadow_size_) * shadow_size_;
        // 1.0 is the far plane here, so an untouched texel can never occlude.
        std::vector<float> far_colour(texels * 4, 1.0f);
        if (!Write(shadow_colour_, far_colour.data(), far_colour.size() * sizeof(float))) {
            error = "the shadow map could not be cleared";
            return false;
        }
        std::vector<float> far_depth(texels, 1.0f);
        if (!Write(shadow_depth_, far_depth.data(), far_depth.size() * sizeof(float))) {
            error = "the shadow depth target could not be cleared";
            return false;
        }
    }

    profile_.constants_ms = since(constants_started);

    const auto stream_started = probe();

    // --- the stream ---------------------------------------------------------
    const auto format = zlong::gpu::surface::SurfaceFormat::R8G8B8A8_UNORM;
    const auto shadow_format = zlong::gpu::surface::SurfaceFormat::R32G32B32A32_FLOAT;
    const auto depth_format = zlong::gpu::surface::SurfaceFormat::D32_FLOAT;
    const auto linear = zlong::gpu::surface::TileMode::Linear;
    const auto stride = static_cast<std::uint32_t>(sizeof(MeshVertex));

    std::vector<std::uint32_t> words;
    words.reserve((drawable_nodes_.size() + 1) * kWordsPerInstance);

    // The pass's target is `width` by `height`, and the viewport has to be the same shape.
    // It used to be square -- `side` twice -- which stretched every non-square target
    // vertically by width/height, because the projection divides x by the aspect and the
    // viewport then handed that back. A square target hid it; 640x480 and 1024x640 did not.
    const auto emit_pass = [&](GuestPa colour_pa, std::uint32_t width, std::uint32_t height,
                               std::uint32_t colour_pitch,
                               zlong::gpu::surface::SurfaceFormat colour_format, GuestPa depth_pa,
                               std::uint32_t depth_pitch, zlong::gpu::render::CullMode cull) {
        const GpuVAddr colour = OnGpu(colour_pa);
        const GpuVAddr depth = OnGpu(depth_pa);
        Emit(words, zlong::gpu::engine::kMethodColourTarget,
             {Low(colour), High(colour), width, height, colour_pitch,
              static_cast<std::uint32_t>(colour_format), static_cast<std::uint32_t>(linear)});
        Emit(words, zlong::gpu::engine::kMethodDepthTarget,
             {Low(depth), High(depth), width, height, depth_pitch,
              static_cast<std::uint32_t>(depth_format), static_cast<std::uint32_t>(linear)});
        Emit(words, zlong::gpu::engine::kMethodDepthState,
             {1u, 1u, static_cast<std::uint32_t>(zlong::gpu::render::CompareOp::Less)});
        // The meshes wind counter-clockwise seen from outside, which the rasterizer
        // treats as front-facing.
        Emit(words, zlong::gpu::engine::kMethodCull,
             {static_cast<std::uint32_t>(cull),
              static_cast<std::uint32_t>(zlong::gpu::render::FrontFace::CounterClockwise)});
        Emit(words, zlong::gpu::engine::kMethodViewport,
             {Bits(0.0f), Bits(0.0f), Bits(static_cast<float>(width)),
              Bits(static_cast<float>(height)), Bits(0.0f), Bits(1.0f)});
    };

    const auto emit_draw = [&](const UploadedMesh& mesh, std::size_t shader_slot, GuestPa slot) {
        // Per-draw bindings are consumed by the draw, so each drawable binds its
        // own geometry and constants.
        Emit(words, zlong::gpu::engine::kMethodVertexBuffer,
             {Low(OnGpu(mesh.vertices)), High(OnGpu(mesh.vertices)), mesh.vertex_bytes, stride});
        const auto attribute = [&](std::uint32_t location, std::uint32_t offset) {
            Emit(words, zlong::gpu::engine::kMethodVertexAttribute,
                 {location, 0u, offset,
                  static_cast<std::uint32_t>(
                      zlong::gpu::surface::SurfaceFormat::R32G32B32A32_FLOAT),
                  stride});
        };
        attribute(0u, 0u);
        attribute(1u, kNormalOffset);
        attribute(2u, kUvOffset);
        attribute(3u, kTangentOffset);
        attribute(4u, kBitangentOffset);
        Emit(words, zlong::gpu::engine::kMethodShader, {static_cast<std::uint32_t>(shader_slot)});
        Emit(words, zlong::gpu::engine::kMethodIndexBuffer,
             {Low(OnGpu(mesh.indices)), High(OnGpu(mesh.indices)), mesh.index_bytes,
              static_cast<std::uint32_t>(zlong::gpu::render::IndexType::UInt16)});
        Emit(words, zlong::gpu::engine::kMethodIndexCount, {mesh.index_count});
        Emit(words, zlong::gpu::engine::kMethodVertexCount, {mesh.vertex_count});
        Emit(words, zlong::gpu::engine::kMethodConstantBuffer,
             {Low(OnGpu(slot)), High(OnGpu(slot)), sizeof(InstanceConstants), 0u});
        Emit(words, zlong::gpu::engine::kMethodDraw, {0u});
    };

    const auto bind_texture = [&](std::uint32_t slot, GuestPa at,
                                  zlong::gpu::surface::SurfaceFormat texture_format,
                                  std::uint32_t texel_width, std::uint32_t texel_height,
                                  std::uint32_t texel_pitch,
                                  zlong::gpu::SamplerState::Wrap wrap) {
        Emit(words, zlong::gpu::engine::kMethodTexture,
             {Low(OnGpu(at)), High(OnGpu(at)), texel_width, texel_height, texel_pitch,
              static_cast<std::uint32_t>(texture_format), static_cast<std::uint32_t>(linear), slot,
              static_cast<std::uint32_t>(zlong::gpu::SamplerState::Filter::Nearest),
              static_cast<std::uint32_t>(zlong::gpu::SamplerState::Filter::Nearest),
              static_cast<std::uint32_t>(wrap), static_cast<std::uint32_t>(wrap)});
    };

    // The shadow pass runs first and fills the map the main pass samples.
    if (shadows_) {
        // Front faces are culled, so what lands in the map is the far side of each
        // solid. A lit front face then compares as lit no matter how edge-on it is
        // to the light, which is what a constant bias cannot achieve on its own.
        // A single-sided ground is culled out of the map entirely, which is
        // harmless: it has nothing to cast onto.
        emit_pass(shadow_colour_, shadow_size_, shadow_size_, shadow_size_ * 16, shadow_format,
                  shadow_depth_, shadow_size_ * 4, zlong::gpu::render::CullMode::Front);
        for (std::size_t d = 0; d < drawable_nodes_.size(); ++d) {
            const Node& node = scene.nodes[drawable_nodes_[d]];
            emit_draw(meshes_[node.mesh], kShadowDepthShader, shadow_slots_[d]);
        }
    }

    emit_pass(colour_target_, width_, height_, width_ * 4, format, depth_target_, width_ * 4,
              zlong::gpu::render::CullMode::Back);

    for (std::size_t d : visible) {
        const Node& node = scene.nodes[drawable_nodes_[d]];
        const Material& material = scene.materials[node.material];

        const bool textured = material.Textured();
        const bool normal_mapped = material.NormalMapped();
        std::size_t variant = kUntexturedShader;
        if (normal_mapped) {
            variant = shadows_ ? kNormalMappedShadowShader : kNormalMappedShader;
        } else if (textured) {
            variant = shadows_ ? kTexturedShadowShader : kTexturedShader;
        } else {
            variant = shadows_ ? kUntexturedShadowShader : kUntexturedShader;
        }

        if (textured) {
            const Texture& texture = scene.textures[material.texture];
            bind_texture(0u, textures_[material.texture], format, texture.width, texture.height,
                         texture.width * 4, zlong::gpu::SamplerState::Wrap::Repeat);
        }
        if (normal_mapped) {
            const Texture& texture = scene.textures[material.normal_texture];
            bind_texture(1u, textures_[material.normal_texture], format, texture.width,
                         texture.height, texture.width * 4,
                         zlong::gpu::SamplerState::Wrap::Repeat);
        }
        if (shadows_) {
            // Every draw consumes its bindings, so this has to be re-emitted per
            // drawable like the others. Clamped, not repeated: a fragment outside
            // the light's frustum must read the far-plane texels rather than wrap
            // to the other side of the map.
            bind_texture(kShadowTextureSlot, shadow_colour_, shadow_format, shadow_size_,
                         shadow_size_, shadow_size_ * 16, zlong::gpu::SamplerState::Wrap::Clamp);
        }
        emit_draw(meshes_[node.mesh], variant, constant_slots_[d]);
    }

    if (words.size() > stream_capacity_) {
        error = "the command stream outgrew the room Prepare set aside for it";
        return false;
    }
    const auto stream_bytes = static_cast<std::uint64_t>(words.size() * sizeof(std::uint32_t));
    if (!Write(stream_, words.data(), static_cast<std::size_t>(stream_bytes))) {
        error = "the command stream could not be written";
        return false;
    }

    profile_.stream_ms = since(stream_started);

    const auto submit_started = probe();
    const auto result = gpu_.SubmitPushbuffer(stream_, stream_bytes);
    profile_.submit_ms = since(submit_started);
    submitted_ = visible.size();
    draws = result.draws;
    if (!result.ok()) {
        error = std::string(zlong::gpu::ToString(result.status)) + ": " + result.detail;
        return false;
    }
    error.clear();
    return true;
}

GpuVAddr Renderer::OnGpu(GuestPa pa) const {
    // Prepare mapped the arena, so every address the renderer emits is in it. A miss
    // would mean an address from outside the arena, which shows up as the decoder
    // refusing the stream rather than as silently wrong pixels.
    return gpu_.address_space().AddressOf(pa).value_or(0);
}

const std::uint8_t* Renderer::colour_pixels() const {
    return gpu_.memory().peek(colour_target_, static_cast<std::size_t>(width_) * height_ * 4);
}

}  // namespace zlong::engine
