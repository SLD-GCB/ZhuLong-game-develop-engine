#include "zlong/gpu/vulkan/vk_renderer.h"

#include <cstring>

#include "zlong/gpu/shader/spirv_emitter.h"
#include "zlong/gpu/surface/layout.h"
#include "zlong/gpu/surface/texel.h"
#include "zlong/gpu/vulkan/vk_format.h"

namespace zlong::gpu::vulkan {

namespace {

using render::RenderResult;
using render::RenderStatus;

/// Texture slot N is bound at descriptor binding 1 + N, matching what the
/// SPIR-V emitter decorates. Three slots is what the engine's shading needs
/// today: albedo, a tangent-space normal map, and a shadow map.
constexpr std::uint32_t kTextureBindings = 3;

std::uint64_t PipelineKey(const render::DrawDesc& draw, VkFormat colour_format,
                          VkFormat depth_format) {
    std::uint64_t key = 0xCBF2'9CE4'8422'2325ull;
    const auto mix = [&key](std::uint64_t value) {
        key = (key ^ value) * 0x0000'0100'0000'01B3ull;
    };
    mix(draw.vertex_shader->signature);
    mix(draw.fragment_shader->signature);
    mix(static_cast<std::uint64_t>(colour_format));
    mix(static_cast<std::uint64_t>(depth_format));
    mix(static_cast<std::uint64_t>(draw.topology));
    mix(draw.blend.enable ? 1ull : 0ull);
    mix(draw.blend.write_mask);
    mix((static_cast<std::uint64_t>(draw.blend.src_color) << 8) |
        static_cast<std::uint64_t>(draw.blend.dst_color));
    // Depth compare and the write enable are baked into the pipeline, not dynamic
    // state, so a cache key that omits them hands a shadow pass the depth state of
    // whatever was drawn last.
    mix(draw.depth_state.test_enable ? 1ull : 0ull);
    mix(draw.depth_state.write_enable ? 1ull : 0ull);
    mix(static_cast<std::uint64_t>(draw.depth_state.compare));
    mix((static_cast<std::uint64_t>(draw.raster.cull) << 8) |
        static_cast<std::uint64_t>(draw.raster.front_face));
    for (const auto& attribute : draw.attributes) {
        mix(attribute.location);
        mix(attribute.buffer_index);
        mix(attribute.offset);
        mix(static_cast<std::uint64_t>(attribute.format));
        mix(attribute.stride);
    }
    return key;
}

VkBlendFactor ToVkBlendFactor(render::BlendFactor factor) {
    switch (factor) {
    case render::BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case render::BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case render::BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case render::BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case render::BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case render::BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case render::BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case render::BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case render::BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case render::BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp ToVkBlendOp(render::BlendOp op) {
    switch (op) {
    case render::BlendOp::Add: return VK_BLEND_OP_ADD;
    case render::BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
    case render::BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case render::BlendOp::Min: return VK_BLEND_OP_MIN;
    case render::BlendOp::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkCompareOp ToVkCompareOp(render::CompareOp op) {
    switch (op) {
    case render::CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case render::CompareOp::Less: return VK_COMPARE_OP_LESS;
    case render::CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case render::CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case render::CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case render::CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case render::CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case render::CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkCullModeFlags ToVkCullMode(render::CullMode mode) {
    switch (mode) {
    case render::CullMode::None: return VK_CULL_MODE_NONE;
    case render::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case render::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

/// The engine's winding convention carries over unchanged. Even though the viewport
/// flips Y (see the negative height at vkCmdSetViewport), Vulkan decides
/// front-facing from the clip-space winding, so no swap belongs here. Inverting
/// this instead makes the outward faces get culled -- invisible on a closed solid,
/// but it deletes a single-sided mesh like the ground outright.
VkFrontFace ToVkFrontFace(render::FrontFace face) {
    return face == render::FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE
                                                : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkFilter ToVkFilter(SamplerState::Filter filter) {
    return filter == SamplerState::Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkSamplerAddressMode ToVkWrap(SamplerState::Wrap wrap) {
    switch (wrap) {
    case SamplerState::Wrap::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case SamplerState::Wrap::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case SamplerState::Wrap::Clamp:
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

bool CreateTextureSampler(VkDevice device, const SamplerState& state, VkSampler& sampler) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = ToVkFilter(state.mag_filter);
    info.minFilter = ToVkFilter(state.min_filter);
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = ToVkWrap(state.wrap_u);
    info.addressModeV = ToVkWrap(state.wrap_v);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(device, &info, nullptr, &sampler) == VK_SUCCESS;
}

}  // namespace

VulkanBackend::VulkanBackend(VkContext& context, GpuMemoryManager& guest)
    : context_(context), guest_(guest) {}

VulkanBackend::~VulkanBackend() {
    Shutdown();
}

render::BackendCaps VulkanBackend::caps() const noexcept {
    render::BackendCaps caps;
    caps.textures = true;
    caps.depth_test = true;
    caps.blending = true;
    caps.tiled_targets = true;  // via the layout swizzle on writeback
    caps.max_color_attachments = 1;
    return caps;
}

bool VulkanBackend::Initialize(std::string& error) {
    Shutdown();

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context_.queue_family();
    if (vkCreateCommandPool(context_.device(), &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(context_.device(), &fence_info, nullptr, &fence_) != VK_SUCCESS) {
        error = "vkCreateFence failed";
        Shutdown();
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[1 + kTextureBindings]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (std::uint32_t slot = 0; slot < kTextureBindings; ++slot) {
        bindings[1 + slot].binding = 1 + slot;
        bindings[1 + slot].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1 + slot].descriptorCount = 1;
        bindings[1 + slot].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 1 + kTextureBindings;
    set_layout_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(context_.device(), &set_layout_info, nullptr,
                                    &descriptor_set_layout_) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed";
        Shutdown();
        return false;
    }

    VkDescriptorPoolSize pool_sizes[1 + kTextureBindings]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 256;
    for (std::uint32_t slot = 0; slot < kTextureBindings; ++slot) {
        pool_sizes[1 + slot].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[1 + slot].descriptorCount = 256;
    }
    VkDescriptorPoolCreateInfo descriptor_pool_info{};
    descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool_info.maxSets = 256;
    descriptor_pool_info.poolSizeCount = 1 + kTextureBindings;
    descriptor_pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(context_.device(), &descriptor_pool_info, nullptr,
                               &descriptor_pool_) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        Shutdown();
        return false;
    }

    // Placeholder resources: a 1x1 white image and a zeroed UBO, so a draw that
    // uses neither still has both bindings written.
    {
        void* ubo_mapped = nullptr;
        if (!CreateStagingBuffer(16, dummy_ubo_, dummy_ubo_memory_, &ubo_mapped, error)) {
            Shutdown();
            return false;
        }
        std::memset(ubo_mapped, 0, 16);

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {1, 1, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(context_.device(), &image_info, nullptr, &dummy_image_) != VK_SUCCESS) {
            error = "vkCreateImage failed for the placeholder texture";
            Shutdown();
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(context_.device(), dummy_image_, &requirements);
        const std::uint32_t type = context_.FindMemoryType(requirements.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (type == UINT32_MAX ||
            vkAllocateMemory(context_.device(), &allocate, nullptr, &dummy_image_memory_) !=
                VK_SUCCESS ||
            vkBindImageMemory(context_.device(), dummy_image_, dummy_image_memory_, 0) !=
                VK_SUCCESS) {
            error = "allocating the placeholder texture failed";
            Shutdown();
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = dummy_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(context_.device(), &view_info, nullptr, &dummy_image_view_) !=
            VK_SUCCESS) {
            error = "vkCreateImageView failed for the placeholder texture";
            Shutdown();
            return false;
        }
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(context_.device(), &sampler_info, nullptr, &dummy_sampler_) !=
            VK_SUCCESS) {
            error = "vkCreateSampler failed for the placeholder texture";
            Shutdown();
            return false;
        }
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_set_layout_;
    if (vkCreatePipelineLayout(context_.device(), &layout_info, nullptr, &pipeline_layout_) !=
        VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed";
        Shutdown();
        return false;
    }

    // Guest RAM as Vulkan sees it: the buffers the renderer binds are windows onto
    // this, not copies it made.
    //
    // The mode is forced to Imported rather than left to the probe's default. The
    // default prefers a device-local copy because imported memory here is only
    // host-visible (PCIe) -- a good trade for something the GPU reads over and over,
    // and a bad one for what a renderer actually binds: vertex, index and constant
    // data the engine uploads once and each draw reads once. Copying that into device
    // memory costs an allocation and a transfer per range; letting the GPU fetch it
    // in place costs neither. On unified memory the question does not arise at all,
    // and if the import cannot be created VkMemory falls back to Mirrored on its own.
    VkMemory::Options memory_options;
    memory_options.force_mode = GuestMemoryMode::Imported;
    memory_ = std::make_unique<VkMemory>(context_, guest_.physical(), memory_options);
    memory_->set_memory_manager(&guest_);

    ready_ = true;
    error.clear();
    return true;
}

void VulkanBackend::Shutdown() {
    // An open pass holds device objects; drop them before the device goes away.
    AbandonPass();
    ReleaseBuffers();
    memory_.reset();

    const VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE) {
        for (auto& [address, entry] : depth_targets_) {
            if (entry.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, entry.view, nullptr);
            }
            if (entry.image != VK_NULL_HANDLE) {
                vkDestroyImage(device, entry.image, nullptr);
            }
            if (entry.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, entry.memory, nullptr);
            }
        }
        depth_targets_.clear();
        if (dummy_sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device, dummy_sampler_, nullptr);
        }
        if (dummy_image_view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device, dummy_image_view_, nullptr);
        }
        if (dummy_image_ != VK_NULL_HANDLE) {
            vkDestroyImage(device, dummy_image_, nullptr);
        }
        if (dummy_image_memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device, dummy_image_memory_, nullptr);
        }
        if (dummy_ubo_memory_ != VK_NULL_HANDLE) {
            vkUnmapMemory(device, dummy_ubo_memory_);
            vkFreeMemory(device, dummy_ubo_memory_, nullptr);
        }
        if (dummy_ubo_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, dummy_ubo_, nullptr);
        }
        dummy_sampler_ = VK_NULL_HANDLE;
        dummy_image_view_ = VK_NULL_HANDLE;
        dummy_image_ = VK_NULL_HANDLE;
        dummy_image_memory_ = VK_NULL_HANDLE;
        dummy_ubo_memory_ = VK_NULL_HANDLE;
        dummy_ubo_ = VK_NULL_HANDLE;
        for (const VkPipeline pipeline : owned_pipelines_) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        for (const VkShaderModule module : owned_modules_) {
            vkDestroyShaderModule(device, module, nullptr);
        }
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
        }
        if (descriptor_set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptor_set_layout_, nullptr);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
        }
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device, fence_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, command_pool_, nullptr);
        }
    }
    owned_pipelines_.clear();
    owned_modules_.clear();
    modules_.clear();
    pipelines_.clear();
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    command_pool_ = VK_NULL_HANDLE;
    ready_ = false;
}

bool VulkanBackend::Supports(const render::DrawDesc& draw, std::string& why) const {
    if (draw.vertex_shader == nullptr || draw.fragment_shader == nullptr) {
        why = "a vertex and a fragment shader are both required";
        return false;
    }
    if (draw.topology != render::PrimitiveTopology::Triangles) {
        why = "only a triangle list is implemented";
        return false;
    }
    if (draw.index_type != render::IndexType::None) {
        if (draw.index.size == 0) {
            why = "an index type was set but no index buffer is bound";
            return false;
        }
    }
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    if (draw.depth.has_value()) {
        if (!surface::IsDepthTarget(draw.depth->format)) {
            why = "the depth target format is not usable";
            return false;
        }
        depth_format = ToVkFormat(draw.depth->format);
        if (depth_format == VK_FORMAT_UNDEFINED) {
            why = "the depth target format has no Vulkan equivalent";
            return false;
        }
        if (draw.depth->tile != surface::TileMode::Linear) {
            why = "tiled depth targets are not implemented yet";
            return false;
        }
    }
    (void)depth_format;
    const surface::FormatInfo info = surface::DecodeFormat(draw.color.format);
    if (info.bytes_per_pixel == 0) {
        why = "the colour target format is not usable";
        return false;
    }
    if (info.needs_conversion) {
        why = "colour formats that need a byte-order conversion are not implemented yet";
        return false;
    }
    if (draw.vertex_buffers.size() != 1) {
        why = "exactly one vertex buffer is supported so far";
        return false;
    }
    for (const auto& attribute : draw.attributes) {
        if (attribute.buffer_index != 0) {
            why = "attributes must come from vertex buffer 0";
            return false;
        }
    }
    for (const auto& constant : draw.constants) {
        if (constant.slot != 0) {
            why = "only constant buffer slot 0 is implemented on the Vulkan backend";
            return false;
        }
    }
    for (const auto& texture : draw.textures) {
        if (texture.slot >= kTextureBindings) {
            why = "only texture slots 0 and 1 are implemented on the Vulkan backend";
            return false;
        }
    }
    why.clear();
    return true;
}

bool VulkanBackend::Flush(std::string& error) {
    // The end of a submission is the end of the last pass: submit it, wait for it,
    // and publish its targets to guest memory. This is the only point at which the
    // frame's last target has to be current, which is what lets a pass keep its
    // images resident instead of round-tripping them per draw.
    const bool ok = EndPass(error);
    // The cached buffers mirror guest ranges that the next submission may rewrite
    // (per-drawable constants change every frame), so they do not outlive the
    // submission.
    ReleaseBuffers();
    return ok;
}

VkShaderModule VulkanBackend::GetShaderModule(const shader::Module& module, std::string& error) {
    const auto existing = modules_.find(module.signature);
    if (existing != modules_.end()) {
        return existing->second.module;
    }

    std::string spirv_error;
    const auto words = shader::EmitSpirv(module, spirv_error);
    if (!words.has_value()) {
        error = spirv_error;
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = words->size() * sizeof(std::uint32_t);
    info.pCode = words->data();

    VkShaderModule handle = VK_NULL_HANDLE;
    if (vkCreateShaderModule(context_.device(), &info, nullptr, &handle) != VK_SUCCESS) {
        error = "vkCreateShaderModule rejected the emitted SPIR-V";
        return VK_NULL_HANDLE;
    }
    modules_[module.signature] = ModuleEntry{handle};
    owned_modules_.push_back(handle);
    return handle;
}

VkPipeline VulkanBackend::GetPipeline(const render::DrawDesc& draw, VkShaderModule vs,
                                      VkShaderModule fs, VkFormat colour_format,
                                      VkFormat depth_format, std::string& error) {
    const std::uint64_t key = PipelineKey(draw, colour_format, depth_format);
    const auto existing = pipelines_.find(key);
    if (existing != pipelines_.end()) {
        return existing->second;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    const VkVertexInputBindingDescription binding{0, draw.vertex_buffers[0].stride,
                                                  VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(draw.attributes.size());
    for (const auto& attribute : draw.attributes) {
        attributes.push_back(VkVertexInputAttributeDescription{
            attribute.location, 0, ToVkFormat(attribute.format), attribute.offset});
    }

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = ToVkCullMode(draw.raster.cull);
    raster.frontFace = ToVkFrontFace(draw.raster.front_face);
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = draw.blend.enable ? VK_TRUE : VK_FALSE;
    blend_attachment.srcColorBlendFactor = ToVkBlendFactor(draw.blend.src_color);
    blend_attachment.dstColorBlendFactor = ToVkBlendFactor(draw.blend.dst_color);
    blend_attachment.colorBlendOp = ToVkBlendOp(draw.blend.color_op);
    blend_attachment.srcAlphaBlendFactor = ToVkBlendFactor(draw.blend.src_alpha);
    blend_attachment.dstAlphaBlendFactor = ToVkBlendFactor(draw.blend.dst_alpha);
    blend_attachment.alphaBlendOp = ToVkBlendOp(draw.blend.alpha_op);
    blend_attachment.colorWriteMask = draw.blend.write_mask;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = draw.depth_state.test_enable ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = draw.depth_state.write_enable ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = ToVkCompareOp(draw.depth_state.compare);

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colour_format;
    rendering.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
        error = "vkCreateGraphicsPipelines failed";
        return VK_NULL_HANDLE;
    }
    pipelines_[key] = pipeline;
    owned_pipelines_.push_back(pipeline);
    return pipeline;
}

bool VulkanBackend::CreateStagingBuffer(VkDeviceSize size, VkBuffer& buffer,
                                        VkDeviceMemory& memory, void** mapped,
                                        std::string& error) {
    const VkDevice device = context_.device();

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
        error = "vkCreateBuffer failed";
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const std::uint32_t type = context_.FindMemoryType(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        error = "no host-visible coherent memory type";
        return false;
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        error = "vkAllocateMemory failed";
        return false;
    }
    if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        error = "vkBindBufferMemory failed";
        return false;
    }
    if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        error = "vkMapMemory failed";
        return false;
    }
    return true;
}

void VulkanBackend::DestroyStagingBuffer(VkBuffer buffer, VkDeviceMemory memory) {
    if (buffer == VK_NULL_HANDLE && memory == VK_NULL_HANDLE) {
        return;
    }
    const VkDevice device = context_.device();
    if (memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device, memory);
        vkFreeMemory(device, memory, nullptr);
    }
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer, nullptr);
    }
}

bool VulkanBackend::PublishToGuest(const render::RenderTarget& target,
                                   const std::uint8_t* pixels, std::string& error) {
    const surface::FormatInfo info = surface::DecodeFormat(target.format);
    if (info.bytes_per_pixel == 0) {
        error = "unsupported colour format";
        return false;
    }

    surface::Surface surface;
    surface.width = target.width;
    surface.height = target.height;
    surface.pitch = target.pitch;
    surface.tile = target.tile;
    surface.bpp = info.bytes_per_pixel;

    const std::size_t row_bytes = static_cast<std::size_t>(target.width) * info.bytes_per_pixel;
    publication_.assign(surface::SurfaceSize(surface), 0);

    if (surface.tile == surface::TileMode::Linear) {
        const std::size_t pitch = surface.pitch < row_bytes ? row_bytes : surface.pitch;
        for (std::uint32_t y = 0; y < surface.height; ++y) {
            std::memcpy(publication_.data() + static_cast<std::size_t>(y) * pitch,
                        pixels + static_cast<std::size_t>(y) * row_bytes, row_bytes);
        }
    } else {
        surface::SwizzleIn({pixels, row_bytes * surface.height}, surface, publication_);
    }

    if (!guest_.GuestWrite(target.pa, publication_.data(), publication_.size())) {
        error = "guest framebuffer write failed";
        return false;
    }
    return true;
}

VulkanBackend::DepthEntry* VulkanBackend::AcquireDepth(const render::DrawDesc& draw,
                                                       std::string& error) {
    const render::RenderTarget& target = *draw.depth;
    const VkFormat format = ToVkFormat(target.format);
    const VkDevice device = context_.device();

    const auto existing = depth_targets_.find(target.pa);
    if (existing != depth_targets_.end()) {
        DepthEntry& entry = existing->second;
        if (entry.width == target.width && entry.height == target.height &&
            entry.format == format) {
            return &entry;
        }
        if (entry.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, entry.view, nullptr);
        }
        if (entry.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, entry.image, nullptr);
        }
        if (entry.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, entry.memory, nullptr);
        }
        existing->second = DepthEntry{};
    }

    DepthEntry entry;
    entry.width = target.width;
    entry.height = target.height;
    entry.format = format;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {target.width, target.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &image_info, nullptr, &entry.image) != VK_SUCCESS) {
        error = "vkCreateImage failed for the depth target";
        return nullptr;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, entry.image, &requirements);
    const std::uint32_t type = context_.FindMemoryType(requirements.memoryTypeBits,
                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        error = "no device-local memory type for the depth target";
        return nullptr;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocate, nullptr, &entry.memory) != VK_SUCCESS) {
        error = "vkAllocateMemory failed for the depth target";
        return nullptr;
    }
    if (vkBindImageMemory(device, entry.image, entry.memory, 0) != VK_SUCCESS) {
        error = "vkBindImageMemory failed for the depth target";
        return nullptr;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = entry.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view_info, nullptr, &entry.view) != VK_SUCCESS) {
        error = "vkCreateImageView failed for the depth target";
        return nullptr;
    }

    const auto [position, inserted] =
        depth_targets_.insert_or_assign(target.pa, std::move(entry));
    (void)inserted;
    return &position->second;
}

bool VulkanBackend::LoadLinearFromGuest(const render::RenderTarget& target,
                                        std::vector<std::uint8_t>& out) {
    const surface::FormatInfo info = surface::DecodeFormat(target.format);
    if (info.bytes_per_pixel == 0) {
        return false;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(target.width) * info.bytes_per_pixel;
    const std::size_t total = row_bytes * target.height;
    out.assign(total, 0);

    surface::Surface surface;
    surface.width = target.width;
    surface.height = target.height;
    surface.pitch = target.pitch;
    surface.tile = target.tile;
    surface.bpp = info.bytes_per_pixel;

    if (surface.tile == surface::TileMode::Linear) {
        for (std::uint32_t y = 0; y < target.height; ++y) {
            const std::uint8_t* row = guest_.peek(
                target.pa + static_cast<std::uint64_t>(y) *
                                (surface.pitch < row_bytes ? row_bytes : surface.pitch),
                row_bytes);
            if (row == nullptr) {
                return false;
            }
            std::memcpy(out.data() + static_cast<std::size_t>(y) * row_bytes, row, row_bytes);
        }
        return true;
    }

    const std::vector<std::uint8_t> stored = [&] {
        std::vector<std::uint8_t> buffer(surface::SurfaceSize(surface), 0);
        const std::uint8_t* source = guest_.peek(target.pa, buffer.size());
        if (source != nullptr) {
            std::memcpy(buffer.data(), source, buffer.size());
        }
        return buffer;
    }();
    surface::SwizzleOut(stored, surface, out);
    return true;
}

bool VulkanBackend::UploadImage(VkImage image, std::uint32_t width, std::uint32_t height,
                                VkImageAspectFlags aspect, VkImageLayout final_layout,
                                const std::uint8_t* bytes, std::uint64_t size) {
    const VkDevice device = context_.device();
    if (bytes == nullptr) {
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    std::string error;
    if (!CreateStagingBuffer(size, staging, staging_memory, &mapped, error)) {
        return false;
    }
    std::memcpy(mapped, bytes, static_cast<std::size_t>(size));

    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = command_pool_;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &command_info, &commands) != VK_SUCCESS) {
        DestroyStagingBuffer(staging, staging_memory);
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {aspect, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(commands, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = final_layout;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = aspect == VK_IMAGE_ASPECT_COLOR_BIT
                                ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         aspect == VK_IMAGE_ASPECT_COLOR_BIT
                             ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             : VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(commands);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    vkResetFences(device, 1, &fence_);
    const bool ok = vkQueueSubmit(context_.queue(), 1, &submit, fence_) == VK_SUCCESS &&
                    vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX) == VK_SUCCESS;

    vkFreeCommandBuffers(device, command_pool_, 1, &commands);
    DestroyStagingBuffer(staging, staging_memory);
    return ok;
}

bool VulkanBackend::UploadDepth(DepthEntry& entry, GuestPa pa, std::uint64_t size,
                                std::string& error) {
    std::vector<std::uint8_t> source(static_cast<std::size_t>(size), 0);
    const std::uint8_t* mapped = guest_.peek(pa, size);
    if (mapped == nullptr) {
        error = "depth target is not mapped guest memory";
        return false;
    }
    std::memcpy(source.data(), mapped, source.size());
    if (!UploadImage(entry.image, entry.width, entry.height, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, source.data(), size)) {
        error = "uploading the depth target failed";
        return false;
    }
    entry.initialised = true;
    return true;
}

bool VulkanBackend::DownloadDepth(const DepthEntry& entry, GuestPa pa, std::uint64_t size) {
    const VkDevice device = context_.device();

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    std::string error;
    if (!CreateStagingBuffer(size, staging, staging_memory, &mapped, error)) {
        return false;
    }

    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = command_pool_;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &command_info, &commands) != VK_SUCCESS) {
        DestroyStagingBuffer(staging, staging_memory);
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = entry.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.imageExtent = {entry.width, entry.height, 1};
    vkCmdCopyImageToBuffer(commands, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                           &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    vkEndCommandBuffer(commands);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    vkResetFences(device, 1, &fence_);
    const bool ok = vkQueueSubmit(context_.queue(), 1, &submit, fence_) == VK_SUCCESS &&
                    vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX) == VK_SUCCESS;

    bool published = false;
    if (ok) {
        published = guest_.GuestWrite(pa, mapped, static_cast<std::size_t>(size));
    }

    vkFreeCommandBuffers(device, command_pool_, 1, &commands);
    DestroyStagingBuffer(staging, staging_memory);
    return published;
}

bool VulkanBackend::CreateTexture(const TextureBinding& binding, VkImage& image,
                                  VkDeviceMemory& memory, VkImageView& view, VkSampler& sampler,
                                  std::string& error) {
    const surface::FormatInfo source_info = surface::DecodeFormat(binding.texture.format);
    if (source_info.bytes_per_pixel == 0 || binding.texture.width == 0 ||
        binding.texture.height == 0) {
        error = "the texture format or size is not usable";
        return false;
    }

    // Float sources keep a float target: squeezing a shadow map into 8 bits per
    // channel throws away the precision the comparison needs. Everything else is
    // converted to linear RGBA8 on the host, which is one path for every guest
    // format and layout, at the cost of a copy.
    const surface::SurfaceFormat target_format =
        source_info.kind == surface::ComponentKind::Float
            ? surface::SurfaceFormat::R32G32B32A32_FLOAT
            : surface::SurfaceFormat::R8G8B8A8_UNORM;
    const surface::FormatInfo target_info = surface::DecodeFormat(target_format);
    const std::uint32_t target_bytes = target_info.bytes_per_pixel;

    surface::Surface surface;
    surface.width = binding.texture.width;
    surface.height = binding.texture.height;
    surface.pitch = binding.texture.pitch;
    surface.tile = binding.texture.tile;
    surface.bpp = source_info.bytes_per_pixel;

    std::vector<std::uint8_t> linear(
        static_cast<std::size_t>(binding.texture.width) * binding.texture.height * target_bytes, 0);
    for (std::uint32_t y = 0; y < binding.texture.height; ++y) {
        for (std::uint32_t x = 0; x < binding.texture.width; ++x) {
            const std::size_t offset = surface::OffsetOf(surface, x, y);
            if (offset == surface::kInvalidOffset) {
                continue;
            }
            const std::uint8_t* texel =
                guest_.peek(binding.texture.pa + offset, source_info.bytes_per_pixel);
            if (texel == nullptr) {
                error = "the texture is not mapped guest memory";
                return false;
            }
            const std::array<float, 4> rgba = surface::DecodeTexel(source_info, texel);
            surface::EncodeTexel(target_info, rgba,
                                 linear.data() + (static_cast<std::size_t>(y) *
                                                      binding.texture.width + x) * target_bytes);
        }
    }

    const VkFormat texture_format = ToVkFormat(target_format);
    const VkDevice device = context_.device();
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = texture_format;
    image_info.extent = {binding.texture.width, binding.texture.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) {
        error = "vkCreateImage failed for the texture";
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    const std::uint32_t type = context_.FindMemoryType(requirements.memoryTypeBits,
                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        error = "no device-local memory type for the texture";
        return false;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        error = "allocating the texture image failed";
        return false;
    }
    if (!UploadImage(image, binding.texture.width, binding.texture.height,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     linear.data(), linear.size())) {
        error = "uploading the texture failed";
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = texture_format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
        error = "vkCreateImageView failed for the texture";
        return false;
    }

    if (!CreateTextureSampler(device, binding.sampler, sampler)) {
        error = "vkCreateSampler failed for the texture";
        return false;
    }
    return true;
}

bool VulkanBackend::CreateTextureFromGuest(const TextureBinding& binding, CachedTexture& out,
                                           std::string& error) {
    const surface::FormatInfo source = surface::DecodeFormat(binding.texture.format);
    const VkFormat format = ToVkFormat(binding.texture.format);
    // What the GPU can do for itself: a linear guest surface in a format it can
    // sample. Tiled surfaces and formats it has no mapping for still need the host
    // to walk them, and CreateTexture is that path.
    if (memory_ == nullptr || source.bytes_per_pixel == 0 || format == VK_FORMAT_UNDEFINED ||
        binding.texture.tile != surface::TileMode::Linear || binding.texture.width == 0 ||
        binding.texture.height == 0) {
        return false;
    }

    const std::uint64_t bytes = static_cast<std::uint64_t>(binding.texture.pitch) *
                                binding.texture.height;
    const std::size_t slot =
        AcquireBuffer(binding.texture.pa, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, error);
    if (slot == kNoBuffer) {
        return false;
    }
    const CachedBuffer& guest = buffers_[slot];
    if (guest.buffer.mirrored) {
        // Mirrored would mean uploading the bytes to a device buffer first, which is
        // exactly the copy this path exists to avoid.
        error.clear();
        return false;
    }

    const VkDevice device = context_.device();
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {binding.texture.width, binding.texture.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &image_info, nullptr, &out.image) != VK_SUCCESS) {
        error = "vkCreateImage failed for the texture";
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, out.image, &requirements);
    const std::uint32_t type = context_.FindMemoryType(requirements.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        error = "no device-local memory type for the texture";
        return false;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocate, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS) {
        error = "allocating the texture image failed";
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view_info, nullptr, &out.view) != VK_SUCCESS) {
        error = "vkCreateImageView failed for the texture";
        return false;
    }
    if (!CreateTextureSampler(device, binding.sampler, out.sampler)) {
        error = "vkCreateSampler failed for the texture";
        return false;
    }

    // The GPU reads the guest bytes straight out of guest RAM. No host decode, no
    // staging copy, and no submission of its own: this rides along in the pass.
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = out.image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy copy{};
    copy.bufferOffset = guest.buffer.offset;
    // Guest row pitch in texels, which is what lets a padded guest surface land in a
    // tight image.
    copy.bufferRowLength = binding.texture.pitch / source.bytes_per_pixel;
    copy.bufferImageHeight = 0;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {0, 0, 0};
    copy.imageExtent = {binding.texture.width, binding.texture.height, 1};
    vkCmdCopyBufferToImage(commands_, guest.buffer.buffer, out.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier to_sampled = to_dst;
    to_sampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_sampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_sampled);
    return true;
}

VulkanBackend::PassKey VulkanBackend::KeyOf(const render::DrawDesc& draw) {
    PassKey key;
    key.colour = draw.color.pa;
    key.depth = draw.depth.has_value() ? draw.depth->pa : 0;
    key.width = draw.color.width;
    key.height = draw.color.height;
    key.colour_format = draw.color.format;
    return key;
}

render::RenderResult VulkanBackend::Execute(const render::DrawDesc& draw) {
    std::string why;
    if (!Supports(draw, why)) {
        return RenderResult::Fail(RenderStatus::UnsupportedState, why);
    }
    if (!ready_) {
        return RenderResult::Fail(RenderStatus::Internal, "the Vulkan backend is not initialised");
    }

    // A different target means a different pass. Closing the open one is what
    // publishes it -- and it has to happen before the next pass opens, because the
    // shadow map the main pass samples is the shadow pass's colour target.
    if (pass_open_ && !(KeyOf(draw) == pass_key_)) {
        std::string error;
        if (!EndPass(error)) {
            return RenderResult::Fail(RenderStatus::BadTarget, error);
        }
    }

    std::string error;
    if (!pass_open_ && !BeginPass(draw, error)) {
        AbandonPass();
        return RenderResult::Fail(RenderStatus::Internal, error);
    }

    const render::RenderResult result = RecordDraw(draw);
    if (!result.ok()) {
        // The recorded commands would leave a hole in the pass, so it is dropped
        // rather than submitted half-drawn.
        AbandonPass();
    }
    return result;
}

/// Create the pass's colour image, bring it and the depth image up to date with
/// guest memory, and open one command buffer that every draw in the pass appends to.
bool VulkanBackend::BeginPass(const render::DrawDesc& draw, std::string& error) {
    const VkDevice device = context_.device();

    pass_key_ = KeyOf(draw);
    pass_colour_ = draw.color;
    pass_clears_ = draw.clear.enabled;
    pass_seeded_ = false;
    pass_has_draws_ = false;
    pass_has_depth_ = false;
    pass_depth_pa_ = 0;
    pass_depth_bytes_ = 0;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = ToVkFormat(draw.color.format);
    image_info.extent = {draw.color.width, draw.color.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &image_info, nullptr, &colour_image_) != VK_SUCCESS) {
        error = "vkCreateImage failed for the colour target";
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, colour_image_, &requirements);
    const std::uint32_t memory_type = context_.FindMemoryType(requirements.memoryTypeBits,
                                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        error = "no device-local memory type for the colour target";
        return false;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocate, nullptr, &colour_memory_) != VK_SUCCESS ||
        vkBindImageMemory(device, colour_image_, colour_memory_, 0) != VK_SUCCESS) {
        error = "allocating the colour target failed";
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = colour_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = ToVkFormat(draw.color.format);
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view_info, nullptr, &colour_view_) != VK_SUCCESS) {
        error = "vkCreateImageView failed for the colour target";
        return false;
    }

    // The engine clears its targets from the host, so guest memory already holds the
    // clear and seeding from it IS the clear -- and it happens once per pass now
    // rather than once per draw.
    if (!pass_clears_) {
        std::vector<std::uint8_t> linear;
        if (!LoadLinearFromGuest(draw.color, linear)) {
            error = "the colour target is not mapped guest memory";
            return false;
        }
        if (!UploadImage(colour_image_, draw.color.width, draw.color.height,
                         VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         linear.data(), linear.size())) {
            error = "seeding the colour target failed";
            return false;
        }
        pass_seeded_ = true;
    }

    if (draw.depth.has_value()) {
        DepthEntry* depth = AcquireDepth(draw, error);
        if (depth == nullptr) {
            return false;
        }
        pass_has_depth_ = true;
        pass_depth_pa_ = draw.depth->pa;
        pass_depth_bytes_ = static_cast<std::uint64_t>(draw.depth->width) * draw.depth->height *
                            surface::DecodeFormat(draw.depth->format).bytes_per_pixel;
        if (!pass_clears_ &&
            !UploadDepth(*depth, draw.depth->pa, pass_depth_bytes_, error)) {
            return false;
        }
    }

    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = command_pool_;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &command_info, &commands_) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commands_, &begin) != VK_SUCCESS) {
        error = "vkBeginCommandBuffer failed";
        return false;
    }

    // One layout transition for the whole pass. A seeded image is already a colour
    // attachment, so the dependency is a transfer write; an image the device will
    // clear is still undefined.
    VkImageMemoryBarrier to_attachment{};
    to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_attachment.oldLayout = pass_seeded_ ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                           : VK_IMAGE_LAYOUT_UNDEFINED;
    to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_attachment.image = colour_image_;
    to_attachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_attachment.srcAccessMask = pass_seeded_ ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
    to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const VkPipelineStageFlags from_stage =
        pass_seeded_ ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    vkCmdPipelineBarrier(commands_, from_stage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_attachment);

    if (pass_has_depth_ && pass_clears_) {
        DepthEntry* depth = AcquireDepth(draw, error);
        if (depth == nullptr) {
            return false;
        }
        VkImageMemoryBarrier init{};
        init.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        init.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        init.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        init.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        init.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        init.image = depth->image;
        init.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        init.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &init);
    }

    pass_textures_.clear();
    pass_buffers_.clear();
    pass_buffer_memories_.clear();
    pass_open_ = true;
    error.clear();
    return true;
}

/// End the command buffer, submit it once, wait, and write both attachments back to
/// guest memory. This is the only place the pass touches guest memory.
bool VulkanBackend::EndPass(std::string& error) {
    error.clear();
    if (!pass_open_) {
        return true;
    }

    const VkDevice device = context_.device();
    DepthEntry* depth = nullptr;
    if (pass_has_depth_) {
        const auto found = depth_targets_.find(pass_depth_pa_);
        if (found != depth_targets_.end()) {
            depth = &found->second;
        }
    }

    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = colour_image_;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_transfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_transfer);

    const std::size_t row_bytes =
        static_cast<std::size_t>(pass_colour_.width) *
        surface::DecodeFormat(pass_colour_.format).bytes_per_pixel;
    const VkDeviceSize colour_bytes = static_cast<VkDeviceSize>(row_bytes) * pass_colour_.height;

    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void* readback_mapped = nullptr;
    if (!CreateStagingBuffer(colour_bytes, readback, readback_memory, &readback_mapped, error)) {
        AbandonPass();
        return false;
    }

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {pass_colour_.width, pass_colour_.height, 1};
    vkCmdCopyImageToBuffer(commands_, colour_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback,
                           1, &copy);

    bool submitted = vkEndCommandBuffer(commands_) == VK_SUCCESS;
    if (submitted) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands_;
        vkResetFences(device, 1, &fence_);
        submitted = vkQueueSubmit(context_.queue(), 1, &submit, fence_) == VK_SUCCESS &&
                    vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }

    bool published = false;
    if (submitted) {
        published = PublishToGuest(pass_colour_, static_cast<const std::uint8_t*>(readback_mapped),
                                   error);
    } else {
        error = "submitting the pass failed";
    }
    DestroyStagingBuffer(readback, readback_memory);

    // The depth writeback builds its own staging and submit; it is once per pass.
    bool depth_published = true;
    if (depth != nullptr) {
        depth_published = DownloadDepth(*depth, pass_depth_pa_, pass_depth_bytes_);
    }

    AbandonPass();
    if (!published || !depth_published) {
        if (error.empty()) {
            error = "the render target writeback failed";
        }
        return false;
    }
    return true;
}

void VulkanBackend::AbandonPass() {
    const VkDevice device = context_.device();
    if (device == VK_NULL_HANDLE) {
        return;
    }
    if (commands_ != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device, command_pool_, 1, &commands_);
        commands_ = VK_NULL_HANDLE;
    }
    for (VkBuffer buffer : pass_buffers_) {
        vkDestroyBuffer(device, buffer, nullptr);
    }
    for (VkDeviceMemory memory : pass_buffer_memories_) {
        vkFreeMemory(device, memory, nullptr);
    }
    pass_buffers_.clear();
    pass_buffer_memories_.clear();

    for (CachedTexture& texture : pass_textures_) {
        DestroyTexture(texture);
    }
    pass_textures_.clear();

    if (colour_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, colour_view_, nullptr);
        colour_view_ = VK_NULL_HANDLE;
    }
    if (colour_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, colour_image_, nullptr);
        colour_image_ = VK_NULL_HANDLE;
    }
    if (colour_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, colour_memory_, nullptr);
        colour_memory_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkResetDescriptorPool(device, descriptor_pool_, 0);
    }

    pass_open_ = false;
    pass_has_draws_ = false;
    pass_has_depth_ = false;
}

std::size_t VulkanBackend::AcquireBuffer(GuestPa pa, std::uint64_t size, VkBufferUsageFlags usage,
                                         std::string& error) {
    if (size == 0) {
        error = "a zero-length guest buffer cannot be bound";
        return kNoBuffer;
    }
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        const CachedBuffer& cached = buffers_[index];
        if (cached.pa == pa && cached.size == size && cached.usage == usage) {
            return index;
        }
    }
    if (memory_ == nullptr) {
        error = "the guest memory layer is not available";
        return kNoBuffer;
    }

    const auto created = memory_->CreateBuffer(pa, size, usage);
    if (!created.has_value()) {
        error = "the guest range cannot be exposed to Vulkan";
        return kNoBuffer;
    }
    CachedBuffer cached;
    cached.pa = pa;
    cached.size = size;
    cached.usage = usage;
    cached.buffer = *created;
    buffers_.push_back(cached);
    return buffers_.size() - 1;
}

bool VulkanBackend::RecordUpload(const CachedBuffer& cached, std::string& error) {
    if (!cached.buffer.mirrored) {
        return true;  // imported: the device already sees the guest bytes
    }

    const std::uint8_t* source = guest_.peek(cached.pa, cached.size);
    if (source == nullptr) {
        error = "the guest range is not mapped";
        return false;
    }

    // The copy goes into the pass's command buffer rather than its own submission,
    // so a pass that binds seventy distinct ranges still submits once.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!CreateStagingBuffer(cached.size, staging, staging_memory, &mapped, error)) {
        return false;
    }
    pass_buffers_.push_back(staging);
    pass_buffer_memories_.push_back(staging_memory);
    std::memcpy(mapped, source, static_cast<std::size_t>(cached.size));

    VkBufferCopy copy{};
    copy.srcOffset = 0;
    copy.dstOffset = cached.buffer.offset;
    copy.size = cached.size;
    vkCmdCopyBuffer(commands_, staging, cached.buffer.buffer, 1, &copy);

    // The stages that will read it need the transfer write to be visible.
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                            VK_ACCESS_UNIFORM_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = cached.buffer.buffer;
    barrier.offset = cached.buffer.offset;
    barrier.size = cached.size;
    vkCmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    return true;
}

void VulkanBackend::ReleaseBuffers() {
    if (memory_ != nullptr) {
        for (CachedBuffer& cached : buffers_) {
            memory_->DestroyBuffer(cached.buffer);
        }
    }
    buffers_.clear();
}

void VulkanBackend::DestroyTexture(CachedTexture& texture) {
    const VkDevice device = context_.device();
    if (texture.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, texture.sampler, nullptr);
        texture.sampler = VK_NULL_HANDLE;
    }
    if (texture.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, texture.view, nullptr);
        texture.view = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, texture.image, nullptr);
        texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, texture.memory, nullptr);
        texture.memory = VK_NULL_HANDLE;
    }
}

VulkanBackend::CachedTexture* VulkanBackend::AcquireTexture(const TextureBinding& binding,
                                                           std::string& error) {
    for (CachedTexture& cached : pass_textures_) {
        if (cached.pa == binding.texture.pa && cached.width == binding.texture.width &&
            cached.height == binding.texture.height && cached.pitch == binding.texture.pitch &&
            cached.format == binding.texture.format &&
            cached.sampler_state.min_filter == binding.sampler.min_filter &&
            cached.sampler_state.mag_filter == binding.sampler.mag_filter &&
            cached.sampler_state.wrap_u == binding.sampler.wrap_u &&
            cached.sampler_state.wrap_v == binding.sampler.wrap_v) {
            return &cached;
        }
    }

    CachedTexture texture;
    texture.pa = binding.texture.pa;
    texture.width = binding.texture.width;
    texture.height = binding.texture.height;
    texture.pitch = binding.texture.pitch;
    texture.format = binding.texture.format;
    texture.sampler_state = binding.sampler;
    // Let the GPU copy the guest bytes straight into the image when it can; anything
    // it cannot express falls back to walking the texels on the host.
    std::string direct_error;
    if (!CreateTextureFromGuest(binding, texture, direct_error)) {
        DestroyTexture(texture);  // whatever the direct path managed to create
        error.clear();
        if (!CreateTexture(binding, texture.image, texture.memory, texture.view, texture.sampler,
                           error)) {
            return nullptr;
        }
    }
    pass_textures_.push_back(texture);
    // The vector may have grown, so hand back the element rather than the local.
    return &pass_textures_.back();
}

render::RenderResult VulkanBackend::RecordDraw(const render::DrawDesc& draw) {
    const VkDevice device = context_.device();
    const VkFormat colour_format = ToVkFormat(draw.color.format);
    const VkFormat depth_format =
        draw.depth.has_value() ? ToVkFormat(draw.depth->format) : VK_FORMAT_UNDEFINED;
    std::string error;

    VkShaderModule vs = GetShaderModule(*draw.vertex_shader, error);
    if (vs == VK_NULL_HANDLE) {
        return RenderResult::Fail(RenderStatus::ShaderError, error);
    }
    VkShaderModule fs = GetShaderModule(*draw.fragment_shader, error);
    if (fs == VK_NULL_HANDLE) {
        return RenderResult::Fail(RenderStatus::ShaderError, error);
    }
    VkPipeline pipeline = GetPipeline(draw, vs, fs, colour_format, depth_format, error);
    if (pipeline == VK_NULL_HANDLE) {
        return RenderResult::Fail(RenderStatus::Internal, error);
    }

    // The depth image belongs to the pass and is already current: it was brought
    // up to date when the pass opened, not once per draw.
    DepthEntry* depth_entry = nullptr;
    if (pass_has_depth_) {
        const auto found = depth_targets_.find(pass_depth_pa_);
        if (found != depth_targets_.end()) {
            depth_entry = &found->second;
        }
    }

    const BufferBinding& vertices = draw.vertex_buffers[0];

    // ---- guest buffers ---------------------------------------------------
    // Bound through the guest-memory layer and cached for the submission: the
    // engine uploads geometry once and every draw binds the same range, so a fresh
    // buffer per draw was work the GPU never needed.
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

    // A failure inside one draw is reported up, and Execute drops the whole pass
    // rather than submitting it with a hole in it -- so there is nothing to unwind
    // here, which is what makes this a plain wrapper.
    const auto fail = [](RenderStatus status, const std::string& detail) {
        return RenderResult::Fail(status, detail);
    };

    const std::size_t vertex_slot =
        AcquireBuffer(vertices.pa, vertices.size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, error);
    if (vertex_slot == kNoBuffer) {
        return fail(RenderStatus::BadGeometry, error);
    }
    if (!RecordUpload(buffers_[vertex_slot], error)) {
        return fail(RenderStatus::Internal, error);
    }

    std::size_t index_slot = kNoBuffer;
    if (draw.index_type != render::IndexType::None) {
        index_slot = AcquireBuffer(draw.index.pa, draw.index.size,
                                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT, error);
        if (index_slot == kNoBuffer) {
            return fail(RenderStatus::BadGeometry, error);
        }
        if (!RecordUpload(buffers_[index_slot], error)) {
            return fail(RenderStatus::Internal, error);
        }
    }

    constexpr VkDeviceSize kUniformBytes = 1024;
    VkBuffer ubo_used = dummy_ubo_;
    VkDeviceSize ubo_offset = 0;
    VkDeviceSize ubo_range = kUniformBytes;
    VkImageView bound_views[kTextureBindings];
    VkSampler bound_samplers[kTextureBindings];
    for (std::uint32_t slot = 0; slot < kTextureBindings; ++slot) {
        bound_views[slot] = dummy_image_view_;
        bound_samplers[slot] = dummy_sampler_;
    }
    if (!draw.constants.empty()) {
        const ConstantBufferBinding& constant = draw.constants.front();
        const std::size_t ubo_slot =
            AcquireBuffer(constant.pa, constant.size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, error);
        if (ubo_slot == kNoBuffer) {
            return fail(RenderStatus::BadGeometry, error);
        }
        if (!RecordUpload(buffers_[ubo_slot], error)) {
            return fail(RenderStatus::Internal, error);
        }
        ubo_used = buffers_[ubo_slot].buffer.buffer;
        ubo_offset = buffers_[ubo_slot].buffer.offset;
        ubo_range = buffers_[ubo_slot].buffer.size;
    }
    for (const TextureBinding& binding : draw.textures) {
        CachedTexture* texture = AcquireTexture(binding, error);
        if (texture == nullptr) {
            return fail(RenderStatus::BadTarget, error);
        }
        // Copied out at once: a later AcquireTexture in this loop can grow the cache
        // and move the element this pointer names.
        bound_views[binding.slot] = texture->view;
        bound_samplers[binding.slot] = texture->sampler;
    }

    {
        VkDescriptorSetAllocateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_info.descriptorPool = descriptor_pool_;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts = &descriptor_set_layout_;
        if (vkAllocateDescriptorSets(device, &set_info, &descriptor_set) != VK_SUCCESS) {
            return fail(RenderStatus::Internal, "vkAllocateDescriptorSets failed");
        }
        VkDescriptorBufferInfo buffer_info{ubo_used, ubo_offset,
                                           draw.constants.empty() ? 16 : ubo_range};
        VkDescriptorImageInfo descriptor_images[kTextureBindings];
        VkWriteDescriptorSet writes[1 + kTextureBindings]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &buffer_info;
        for (std::uint32_t slot = 0; slot < kTextureBindings; ++slot) {
            descriptor_images[slot] = {bound_samplers[slot], bound_views[slot],
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet& write = writes[1 + slot];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptor_set;
            write.dstBinding = 1 + slot;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &descriptor_images[slot];
        }
        vkUpdateDescriptorSets(device, 1 + kTextureBindings, writes, 0, nullptr);
    }

    // ---- record into the pass's command buffer ---------------------------
    // No begin, no barriers, no submit: the pass owns all of that, and the whole
    // pass goes to the queue as one submission at EndPass.
    VkRenderingAttachmentInfo colour_attachment{};
    colour_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour_attachment.imageView = colour_view_;
    colour_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // Only a pass that asked for a device-side clear clears, and only its first draw
    // does -- every later draw must load what the earlier ones wrote.
    const bool clears_here = pass_clears_ && !pass_has_draws_;
    colour_attachment.loadOp = clears_here ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                           : VK_ATTACHMENT_LOAD_OP_LOAD;
    colour_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour_attachment.clearValue.color = {{draw.clear.color[0], draw.clear.color[1],
                                          draw.clear.color[2], draw.clear.color[3]}};

    VkRenderingAttachmentInfo depth_attachment{};
    if (depth_entry != nullptr) {
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = depth_entry->view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = clears_here ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                              : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.clearValue.depthStencil = {draw.clear.depth, draw.clear.stencil};
    }

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {draw.color.width, draw.color.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colour_attachment;
    if (depth_entry != nullptr) {
        rendering.pDepthAttachment = &depth_attachment;
    }

    vkCmdBeginRendering(commands_, &rendering);
    vkCmdBindPipeline(commands_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (descriptor_set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(commands_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                &descriptor_set, 0, nullptr);
    }

    // Vulkan's clip space has +Y down, the engine's has +Y up, so the viewport is
    // given a negative height to flip the axis. A negative height means the origin
    // is the BOTTOM edge, hence y + height and negated height.
    VkViewport viewport{draw.viewport.x, draw.viewport.y + draw.viewport.height,
                        draw.viewport.width, -draw.viewport.height, draw.viewport.min_depth,
                        draw.viewport.max_depth};
    vkCmdSetViewport(commands_, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {draw.color.width, draw.color.height}};
    vkCmdSetScissor(commands_, 0, 1, &scissor);

    // The bindings carry the window's offset: in the imported mode the buffer is the
    // whole RAM region and the guest range sits somewhere inside it.
    const VkBuffer vertex_buffer = buffers_[vertex_slot].buffer.buffer;
    const VkDeviceSize vertex_offset = buffers_[vertex_slot].buffer.offset;
    vkCmdBindVertexBuffers(commands_, 0, 1, &vertex_buffer, &vertex_offset);

    if (draw.index_type != render::IndexType::None) {
        const VkIndexType index_type = draw.index_type == render::IndexType::UInt16
                                           ? VK_INDEX_TYPE_UINT16
                                           : VK_INDEX_TYPE_UINT32;
        vkCmdBindIndexBuffer(commands_, buffers_[index_slot].buffer.buffer,
                             buffers_[index_slot].buffer.offset, index_type);
        vkCmdDrawIndexed(commands_, draw.index_count, 1, draw.first_index, draw.vertex_offset, 0);
    } else {
        vkCmdDraw(commands_, draw.vertex_count, 1,
                  draw.first_vertex + static_cast<std::uint32_t>(draw.vertex_offset), 0);
    }
    vkCmdEndRendering(commands_);

    pass_has_draws_ = true;
    return RenderResult::Good();
}

}  // namespace zlong::gpu::vulkan
