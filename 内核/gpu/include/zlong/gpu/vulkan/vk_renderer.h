// 烛龙 (ZhuLong) - the Vulkan render backend.
//
// Renders into a device-local image and publishes the result into the guest
// framebuffer. Colour targets, depth targets, constant buffers and up to two
// sampled textures are wired up; anything else is reported as unsupported rather
// than mis-rendered.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "zlong/gpu/memory/gpu_memory.h"
#include "zlong/gpu/render/backend.h"
#include "zlong/gpu/shader/ir.h"
#include "zlong/gpu/vulkan/vk_context.h"
#include "zlong/gpu/vulkan/vk_memory.h"

namespace zlong::gpu::vulkan {

class VulkanBackend final : public render::RenderBackend {
public:
    VulkanBackend(VkContext& context, GpuMemoryManager& guest);
    ~VulkanBackend() override;

    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    const char* name() const noexcept override { return "vulkan"; }
    render::BackendCaps caps() const noexcept override;

    bool Initialize(std::string& error) override;
    void Shutdown() override;
    bool ready() const noexcept override { return ready_; }

    render::RenderResult Execute(const render::DrawDesc& draw) override;
    bool Flush(std::string& error) override;
    bool Supports(const render::DrawDesc& draw, std::string& why) const override;

private:
    struct ModuleEntry {
        VkShaderModule module = VK_NULL_HANDLE;
    };

    /// A device-local depth image kept per guest depth surface, so depth persists
    /// across draws the same way the software backend's staged surface does.
    struct DepthEntry {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool initialised = false;
    };

    DepthEntry* AcquireDepth(const render::DrawDesc& draw, std::string& error);
    bool UploadDepth(DepthEntry& entry, GuestPa pa, std::uint64_t size, std::string& error);
    bool DownloadDepth(const DepthEntry& entry, GuestPa pa, std::uint64_t size);

    /// A pass is a run of draws that share one colour target and one depth target --
    /// the shadow pass, then the main pass. Both images stay in device memory for
    /// the whole run and guest memory is touched only when the pass opens and
    /// closes, which is what stops a 65-draw frame from round-tripping every target
    /// 65 times.
    struct PassKey {
        GuestPa colour = 0;
        GuestPa depth = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        surface::SurfaceFormat colour_format = surface::SurfaceFormat::Unknown;

        bool operator==(const PassKey&) const = default;
    };

    /// A texture uploaded for the current pass. Two drawables in one pass usually
    /// share their textures -- and every main-pass drawable samples the same shadow
    /// map -- so decoding one per draw is pure waste.
    struct CachedTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        GuestPa pa = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t pitch = 0;
        surface::SurfaceFormat format = surface::SurfaceFormat::Unknown;
        SamplerState sampler_state{};
    };

    static PassKey KeyOf(const render::DrawDesc& draw);
    bool BeginPass(const render::DrawDesc& draw, std::string& error);
    bool EndPass(std::string& error);
    /// Release the open pass without submitting it. Used when a draw in the middle
    /// of a pass fails: the recorded commands would leave a hole in the result.
    void AbandonPass();
    render::RenderResult RecordDraw(const render::DrawDesc& draw);
    CachedTexture* AcquireTexture(const TextureBinding& binding, std::string& error);
    void DestroyTexture(CachedTexture& texture);

    /// A device buffer standing in for a guest range. Guest geometry and constants
    /// are uploaded once by the engine and then bound by every draw that uses them,
    /// so creating a fresh buffer per draw was pure waste -- and in the imported
    /// memory mode it is not needed at all.
    struct CachedBuffer {
        GuestPa pa = 0;
        std::uint64_t size = 0;
        VkBufferUsageFlags usage = 0;
        GuestBuffer buffer{};
    };

    static constexpr std::size_t kNoBuffer = static_cast<std::size_t>(-1);

    std::size_t AcquireBuffer(GuestPa pa, std::uint64_t size, VkBufferUsageFlags usage,
                              std::string& error);
    /// Record the guest bytes into the buffer, inside the pass command buffer. In
    /// the imported mode there is nothing to record: the device reads guest RAM.
    bool RecordUpload(const CachedBuffer& cached, std::string& error);
    void ReleaseBuffers();

    /// Guest RAM as Vulkan sees it. In the imported mode a buffer aliases the guest
    /// allocation, so the GPU reads guest memory in place and the CPU never touches
    /// the bytes.
    std::unique_ptr<VkMemory> memory_;
    std::vector<CachedBuffer> buffers_;

    bool pass_open_ = false;
    PassKey pass_key_{};
    /// The colour target as guest memory holds it, for the writeback.
    render::RenderTarget pass_colour_{};
    bool pass_seeded_ = false;
    bool pass_clears_ = false;
    bool pass_has_draws_ = false;
    bool pass_has_depth_ = false;
    GuestPa pass_depth_pa_ = 0;
    std::uint64_t pass_depth_bytes_ = 0;
    VkImage colour_image_ = VK_NULL_HANDLE;
    VkDeviceMemory colour_memory_ = VK_NULL_HANDLE;
    VkImageView colour_view_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    /// Everything the recorded command buffer still points at. Released only after
    /// the pass has been submitted and waited on.
    std::vector<CachedTexture> pass_textures_;
    std::vector<VkBuffer> pass_buffers_;
    std::vector<VkDeviceMemory> pass_buffer_memories_;

    /// Seed a freshly created image from a tightly packed linear buffer. Needed
    /// whenever a draw does not clear: the image is created per draw, so without
    /// this the previous contents would be undefined.
    bool UploadImage(VkImage image, std::uint32_t width, std::uint32_t height,
                     VkImageAspectFlags aspect, VkImageLayout final_layout,
                     const std::uint8_t* bytes, std::uint64_t size);

    /// Copy a guest surface into a tightly packed linear buffer, undoing whatever
    /// layout and pitch the guest used.
    bool LoadLinearFromGuest(const render::RenderTarget& target, std::vector<std::uint8_t>& out);

    VkShaderModule GetShaderModule(const shader::Module& module, std::string& error);
    VkPipeline GetPipeline(const render::DrawDesc& draw, VkShaderModule vs, VkShaderModule fs,
                           VkFormat colour_format, VkFormat depth_format, std::string& error);
    bool CreateStagingBuffer(VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory,
                             void** mapped, std::string& error);
    void DestroyStagingBuffer(VkBuffer buffer, VkDeviceMemory memory);
    bool PublishToGuest(const render::RenderTarget& target, const std::uint8_t* pixels,
                        std::string& error);

    VkContext& context_;
    GpuMemoryManager& guest_;
    bool ready_ = false;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    std::map<std::uint64_t, ModuleEntry> modules_;
    std::map<std::uint64_t, VkPipeline> pipelines_;
    std::map<GuestPa, DepthEntry> depth_targets_;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

    /// Placeholders so both descriptor bindings are always written, even when a
    /// draw uses neither a constant buffer nor a texture.
    VkBuffer dummy_ubo_ = VK_NULL_HANDLE;
    VkDeviceMemory dummy_ubo_memory_ = VK_NULL_HANDLE;
    VkImage dummy_image_ = VK_NULL_HANDLE;
    VkDeviceMemory dummy_image_memory_ = VK_NULL_HANDLE;
    VkImageView dummy_image_view_ = VK_NULL_HANDLE;
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;

    /// Create a device-local sampled image from a guest texture, plus a view and
    /// a sampler matching the requested filter and wrap modes.
    bool CreateTexture(const TextureBinding& binding, VkImage& image, VkDeviceMemory& memory,
                       VkImageView& view, VkSampler& sampler, std::string& error);

    /// The same, but handing the guest bytes to the GPU instead of decoding them on
    /// the host: valid only when the guest texture is linear and its format maps to a
    /// Vulkan one, and only in the imported memory mode where the source needs no
    /// copy of its own. `false` means "use CreateTexture instead".
    bool CreateTextureFromGuest(const TextureBinding& binding, CachedTexture& out,
                                std::string& error);
    std::vector<VkShaderModule> owned_modules_;
    std::vector<VkPipeline> owned_pipelines_;

    std::vector<std::uint8_t> publication_;
};

}  // namespace zlong::gpu::vulkan
