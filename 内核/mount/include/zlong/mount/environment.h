// 烛龙 (ZhuLong) - the system environment: what a mounted source set amounts to.
//
// Two inputs, two separate paths, neither guessed from a filename:
//   * a system source -- a host directory, or a host image file -- mounted
//     read-only at "/system";
//   * an entry source -- a host file holding the entry table.
//
// This is the whole of stage one: after LoadEnvironment succeeds, the resources
// are in place and nothing above has to know where they came from.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "zlong/mount/entries.h"
#include "zlong/mount/mount.h"

namespace zlong::mount {

/// The mount name a system source lives at.
inline constexpr std::string_view kSystemMountName = "system";

struct SystemEnvironment {
    MountTable mounts;
    EntryStore entries;

    bool empty() const noexcept { return mounts.mount_count() == 0 && entries.empty(); }
};

struct EnvironmentConfig {
    /// A host directory or a host image file. Empty is an error, not a default.
    std::string system_source;
    /// A host file holding the entry table.
    std::string entry_source;
};

enum class EnvironmentError {
    None,
    /// The config left the system source out.
    SystemSourceMissing,
    /// The config left the entry source out.
    EntrySourceMissing,
    /// A source path names nothing usable.
    SourceUnusable,
    /// The source's contents could not be read as a tree.
    TreeFailed,
    /// The tree could not be mounted.
    MountFailed,
    /// The entry source could not be read as a table.
    EntryFailed,
};

const char* ToString(EnvironmentError error) noexcept;

/// Load both inputs. `detail` receives the specific reason -- the layer's own
/// error code only says which stage failed.
std::optional<SystemEnvironment> LoadEnvironment(const EnvironmentConfig& config,
                                                EnvironmentError* error = nullptr,
                                                std::string* detail = nullptr);

}  // namespace zlong::mount
