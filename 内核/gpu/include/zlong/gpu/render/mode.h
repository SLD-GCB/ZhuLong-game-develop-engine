// 烛龙 (ZhuLong) - render mode selection.
//
// Some machines have only a CPU, so the choice between the GPU and CPU render
// paths is explicit, observable, and never a silent fallback.

#pragma once

#include <cstdint>
#include <string>

namespace zlong::gpu::render {

enum class RenderMode : std::uint8_t {
    /// Pick from the capability probe.
    Auto,
    /// Force the Vulkan backend; fails loudly if it is not usable.
    Vulkan,
    /// Force the software backend.
    Software,
};

enum class ActiveBackend : std::uint8_t { None, Vulkan, Software };

const char* ToString(RenderMode mode) noexcept;
const char* ToString(ActiveBackend backend) noexcept;

/// Neutral capability description.
///
/// Deliberately does NOT reference vulkan::Probe: the CPU-only build excludes
/// the Vulkan sources entirely and must not be forced to include Vulkan headers
/// just to describe what it has.
struct DeviceCapabilities {
    bool device_available = false;
    std::string device_name;
    bool dynamic_rendering = false;
    bool synchronization2 = false;
    bool external_memory_host = false;
    std::uint64_t min_import_alignment = 1;
};

struct ModeDecision {
    RenderMode requested = RenderMode::Auto;
    ActiveBackend chosen = ActiveBackend::None;
    /// Always populated, human readable.
    std::string reason;
    /// Set when a Vulkan attempt failed.
    std::string vulkan_error;

    bool ok() const noexcept { return chosen != ActiveBackend::None; }
};

/// Pure policy: performs no graphics API calls, so it is unit-testable with a
/// synthetic capability description.
///
/// Rules, in order:
///   * requested Software -> Software.
///   * requested Vulkan without a usable device -> NOT Ok (chosen == None), with
///     vulkan_error set. A forced mode never silently degrades.
///   * Auto -> Vulkan when the device has dynamic rendering + synchronization2,
///     otherwise Software, naming the cause.
ModeDecision ChooseMode(RenderMode requested, const DeviceCapabilities& capabilities,
                        std::string vulkan_error);

}  // namespace zlong::gpu::render
