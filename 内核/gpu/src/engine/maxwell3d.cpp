#include "zlong/gpu/engine/maxwell3d.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace zlong::gpu::engine {

namespace {

float FloatOf(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

void Maxwell3D::SetShader(std::size_t slot, const shader::Module* vertex,
                          const shader::Module* fragment) {
    if (slot >= shaders_.size()) {
        shaders_.resize(slot + 1);
    }
    shaders_[slot] = ShaderSlot{vertex, fragment};
}

void Maxwell3D::CopyShadersFrom(const Maxwell3D& other) {
    shaders_ = other.shaders_;
    // Which slot a stream selects is channel state, so it does not come along.
    selected_shader_slot_ = 0;
}

bool Maxwell3D::Expect(const Method& method, std::uint32_t minimum, std::string& error) {
    if (method.argument_count < minimum) {
        error = "method at 0x" + std::to_string(method.address) + " has too few arguments";
        return false;
    }
    return true;
}

bool Maxwell3D::Address(const Method& method, const std::uint32_t* arguments, std::uint64_t& out,
                        std::string& error) const {
    const std::uint64_t name = static_cast<std::uint64_t>(arguments[0]) |
                               (static_cast<std::uint64_t>(arguments[1]) << 32);
    if (address_space_ == nullptr) {
        error = "the engine has no GPU address space to resolve through";
        return false;
    }
    const std::optional<GuestPa> physical = address_space_->Resolve(name);
    if (!physical.has_value()) {
        error = "method at 0x" + std::to_string(method.address) + " names GPU address 0x" +
                std::to_string(name) + ", which was never mapped";
        return false;
    }
    out = *physical;
    return true;
}

bool Maxwell3D::Dispatch(const Method& method, const std::uint32_t* arguments,
                         std::string& error) {
    switch (method.address) {
    case kMethodClearColour:
        if (!Expect(method, 4, error)) {
            return false;
        }
        clear_.color = {FloatOf(arguments[0]), FloatOf(arguments[1]), FloatOf(arguments[2]),
                        FloatOf(arguments[3])};
        break;

    case kMethodClear:
        if (!Expect(method, 1, error)) {
            return false;
        }
        clear_.enabled = arguments[0] != 0;
        break;

    case kMethodColourTarget: {
        if (!Expect(method, 7, error)) {
            return false;
        }
        std::uint64_t physical = 0;
        if (!Address(method, arguments, physical, error)) {
            return false;
        }
        colour_target_.pa = physical;
        colour_target_.width = arguments[2];
        colour_target_.height = arguments[3];
        colour_target_.pitch = arguments[4];
        colour_target_.format = static_cast<surface::SurfaceFormat>(arguments[5]);
        colour_target_.tile = static_cast<surface::TileMode>(arguments[6]);
        has_colour_target_ = true;
        break;
    }

    case kMethodViewport:
        if (!Expect(method, 6, error)) {
            return false;
        }
        viewport_.x = FloatOf(arguments[0]);
        viewport_.y = FloatOf(arguments[1]);
        viewport_.width = FloatOf(arguments[2]);
        viewport_.height = FloatOf(arguments[3]);
        viewport_.min_depth = FloatOf(arguments[4]);
        viewport_.max_depth = FloatOf(arguments[5]);
        break;

    case kMethodDepthTarget: {
        if (!Expect(method, 7, error)) {
            return false;
        }
        render::RenderTarget target;
        std::uint64_t physical = 0;
        if (!Address(method, arguments, physical, error)) {
            return false;
        }
        target.pa = physical;
        target.width = arguments[2];
        target.height = arguments[3];
        target.pitch = arguments[4];
        target.format = static_cast<surface::SurfaceFormat>(arguments[5]);
        target.tile = static_cast<surface::TileMode>(arguments[6]);
        depth_target_ = target;
        break;
    }

    case kMethodDepthState:
        if (!Expect(method, 3, error)) {
            return false;
        }
        depth_state_.test_enable = arguments[0] != 0;
        depth_state_.write_enable = arguments[1] != 0;
        depth_state_.compare = static_cast<render::CompareOp>(arguments[2]);
        break;

    case kMethodCull:
        if (!Expect(method, 2, error)) {
            return false;
        }
        raster_.cull = static_cast<render::CullMode>(arguments[0]);
        raster_.front_face = static_cast<render::FrontFace>(arguments[1]);
        break;

    case kMethodConstantBuffer: {
        if (!Expect(method, 4, error)) {
            return false;
        }
        ConstantBufferBinding binding;
        std::uint64_t physical = 0;
        if (!Address(method, arguments, physical, error)) {
            return false;
        }
        binding.pa = physical;
        binding.size = arguments[2];
        binding.slot = arguments[3];
        // Replace rather than append: a stream re-binds one buffer per object,
        // and two bindings on one slot would make the shader read whichever one
        // happened to be first.
        const auto existing = std::find_if(
            constants_.begin(), constants_.end(),
            [slot = binding.slot](const ConstantBufferBinding& candidate) {
                return candidate.slot == slot;
            });
        if (existing != constants_.end()) {
            *existing = binding;
        } else {
            constants_.push_back(binding);
        }
        break;
    }

    case kMethodVertexBuffer: {
        if (!Expect(method, 4, error)) {
            return false;
        }
        std::uint64_t physical = 0;
        if (!Address(method, arguments, physical, error)) {
            return false;
        }
        vertex_buffers_.push_back(BufferBinding{physical, arguments[2], arguments[3]});
        break;
    }

    case kMethodVertexAttribute: {
        if (!Expect(method, 5, error)) {
            return false;
        }
        render::VertexAttribute attribute;
        attribute.location = arguments[0];
        attribute.buffer_index = arguments[1];
        attribute.offset = arguments[2];
        attribute.format = static_cast<surface::SurfaceFormat>(arguments[3]);
        attribute.stride = arguments[4];
        attributes_.push_back(attribute);
        break;
    }

    case kMethodShader:
        if (!Expect(method, 1, error)) {
            return false;
        }
        selected_shader_slot_ = arguments[0];
        break;

    case kMethodVertexCount:
        if (!Expect(method, 1, error)) {
            return false;
        }
        vertex_count_ = arguments[0];
        break;

    case kMethodIndexBuffer: {
        if (!Expect(method, 4, error)) {
            return false;
        }
        std::uint64_t physical = 0;
        if (!Address(method, arguments, physical, error)) {
            return false;
        }
        index_.pa = physical;
        index_.size = arguments[2];
        index_type_ = static_cast<render::IndexType>(arguments[3]);
        break;
    }

    case kMethodIndexCount:
        if (!Expect(method, 1, error)) {
            return false;
        }
        index_count_ = arguments[0];
        break;

    case kMethodTexture: {
        if (!Expect(method, 12, error)) {
            return false;
        }
        TextureBinding binding;
        std::uint64_t physical = 0;
        if (!Address(method, arguments, physical, error)) {
            return false;
        }
        binding.texture.pa = physical;
        binding.texture.width = arguments[2];
        binding.texture.height = arguments[3];
        binding.texture.pitch = arguments[4];
        binding.texture.format = static_cast<surface::SurfaceFormat>(arguments[5]);
        binding.texture.tile = static_cast<surface::TileMode>(arguments[6]);
        binding.slot = arguments[7];
        binding.sampler.min_filter = static_cast<SamplerState::Filter>(arguments[8]);
        binding.sampler.mag_filter = static_cast<SamplerState::Filter>(arguments[9]);
        binding.sampler.wrap_u = static_cast<SamplerState::Wrap>(arguments[10]);
        binding.sampler.wrap_v = static_cast<SamplerState::Wrap>(arguments[11]);
        // Replace by slot, like a constant buffer: two bindings on one slot would
        // leave the shader reading whichever happened to be first.
        const auto existing = std::find_if(textures_.begin(), textures_.end(),
                                           [slot = binding.slot](const TextureBinding& candidate) {
                                               return candidate.slot == slot;
                                           });
        if (existing != textures_.end()) {
            *existing = std::move(binding);
        } else {
            textures_.push_back(std::move(binding));
        }
        break;
    }

    case kMethodDraw: {
        // A draw with no usable state is silently dropped: the stream degrades
        // rather than failing, and skipped_methods() surfaces the oddity.
        if (!has_colour_target_ || selected_shader_slot_ >= shaders_.size()) {
            break;
        }
        const ShaderSlot& slot = shaders_[selected_shader_slot_];
        if (slot.vertex == nullptr || slot.fragment == nullptr) {
            break;
        }

        render::DrawDesc draw;
        draw.color = colour_target_;
        draw.depth = depth_target_;
        draw.viewport = viewport_;
        draw.clear = clear_;
        draw.raster = raster_;
        draw.depth_state = depth_state_;
        draw.vertex_shader = slot.vertex;
        draw.fragment_shader = slot.fragment;
        draw.vertex_buffers = vertex_buffers_;
        draw.attributes = attributes_;
        draw.constants = constants_;
        draw.textures = textures_;
        draw.vertex_count = vertex_count_;
        draw.vertex_offset = static_cast<std::int32_t>(vertex_offset_);
        draw.index = index_;
        // An index count is what makes a draw indexed; with none the binding is
        // left over rather than meaningful, so it is not passed on.
        draw.index_type = index_count_ != 0 ? index_type_ : render::IndexType::None;
        draw.index_count = index_count_;
        draw.topology = render::PrimitiveTopology::Triangles;
        draw.blend.write_mask = 0x0F;
        pending_ = draw;

        // Geometry, constants and both counts belong to the draw that used them;
        // the pipeline state above does not. A stream that wants two objects says
        // so twice.
        vertex_buffers_.clear();
        attributes_.clear();
        constants_.clear();
        textures_.clear();
        index_ = BufferBinding{};
        index_type_ = render::IndexType::None;
        index_count_ = 0;
        vertex_count_ = 0;
        break;
    }

    default:
        // Unknown methods are counted, not fatal.
        ++skipped_methods_;
        break;
    }

    error.clear();
    return true;
}

std::optional<render::DrawDesc> Maxwell3D::TakeDraw() {
    std::optional<render::DrawDesc> draw = std::move(pending_);
    pending_.reset();
    return draw;
}

void Maxwell3D::Reset() {
    shaders_.clear();
    selected_shader_slot_ = 0;
    skipped_methods_ = 0;
    colour_target_ = render::RenderTarget{};
    has_colour_target_ = false;
    depth_target_.reset();
    clear_ = render::ClearState{};
    viewport_ = render::Viewport{};
    depth_state_ = render::DepthState{};
    raster_ = render::RasterState{};
    vertex_buffers_.clear();
    attributes_.clear();
    constants_.clear();
    textures_.clear();
    index_ = BufferBinding{};
    index_type_ = render::IndexType::None;
    index_count_ = 0;
    vertex_count_ = 0;
    vertex_offset_ = 0;
    pending_.reset();
}

}  // namespace zlong::gpu::engine
