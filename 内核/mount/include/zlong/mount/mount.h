// 烛龙 (ZhuLong) - the mount table: names to read-only trees.
//
// A mounted source is addressed as "<name>/<path inside the tree>". Mount names
// are a single segment, so a path's first segment selects the tree and the rest
// is resolved inside it. The table owns the trees, and each tree keeps its own
// backing devices alive.

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zlong/mount/tree.h"

namespace zlong::mount {

/// Why a mount or a lookup failed. Explicit, because "it did not work" is not a
/// diagnosis -- a bad mount name and a missing mount are different problems.
enum class MountError {
    None,
    /// The name is empty, is "." or "..", or contains '/', '\\' or a NUL.
    BadName,
    /// Something is already mounted at that name.
    AlreadyMounted,
    /// The first segment names no mount.
    NotFound,
    /// The path is empty or malformed (see SplitTreePath).
    BadPath,
    /// The named tree refused the operation; ask the tree for its TreeError.
    Tree,
};

const char* ToString(MountError error) noexcept;

class MountTable {
public:
    MountTable() = default;

    bool Mount(std::string name, ReadOnlyTree tree, MountError* error = nullptr);
    bool Unmount(std::string_view name);
    bool mounted(std::string_view name) const;

    /// Mounted names, sorted.
    std::vector<std::string> mounts() const;
    const ReadOnlyTree* Find(std::string_view name) const;

    struct Resolved {
        const ReadOnlyTree* tree = nullptr;
        /// Path inside that tree; empty means the tree's own root.
        std::string path;
    };

    /// "/system/a/b" -> {tree mounted at "system", "a/b"}. nullopt when the
    /// first segment names no mount, or when the path is malformed.
    std::optional<Resolved> Resolve(std::string_view path, MountError* error = nullptr) const;

    /// Read a whole file through the table.
    std::optional<std::vector<std::uint8_t>> Read(std::string_view path,
                                                 MountError* error = nullptr) const;
    bool Exists(std::string_view path) const;

    std::size_t mount_count() const noexcept { return trees_.size(); }
    /// Total node count across every mount, for diagnostics.
    std::size_t node_count() const noexcept;

private:
    std::map<std::string, ReadOnlyTree, std::less<>> trees_;
};

}  // namespace zlong::mount
