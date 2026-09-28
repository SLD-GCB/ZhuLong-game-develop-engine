// 烛龙 (ZhuLong) - the read-only tree: the mount layer's neutral data model.
//
// A file is a byte range over a block device that the tree keeps alive, not a
// copy: a tree over a large image costs only its node list plus the shared
// device handle. Everything above this layer speaks in terms of this type -- a
// mounted source IS a ReadOnlyTree.
//
// This layer is host-side and runs before any guest does, so it depends on
// nothing above zlong::ssd.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zlong/ssd/block_device.h"

namespace zlong::mount {

/// Why a tree operation failed. Explicit codes, because "it did not work" is
/// not a diagnosis.
enum class TreeError {
    None,
    NotFound,
    NotAFile,
    NotADirectory,
    /// The path tried to leave the tree: a ".." or "." segment, an empty
    /// segment, a leading "//", a backslash, or an embedded NUL.
    BadPath,
    /// The requested range runs past the end of the file.
    OutOfRange,
    /// A directory was used as a file, or the tree has no root.
    NotAForest,
    /// The file node has no backing device.
    NoBacking,
    /// The backing device refused the read.
    DeviceError,
};

const char* ToString(TreeError error) noexcept;

/// Split a tree path into segments.
///
/// Accepts "" and "/" (both mean the root, i.e. zero segments) and one optional
/// leading '/'. Returns nullopt for a malformed path, which is also how path
/// traversal is refused -- ".." never becomes a segment, so it can never be
/// resolved.
std::optional<std::vector<std::string_view>> SplitTreePath(std::string_view path);

/// One node. Directories own their children; a file points at a byte range of a
/// device that must outlive it.
struct Node {
    enum class Kind : std::uint8_t { Directory, File };

    Kind kind = Kind::File;
    std::string name;
    std::vector<Node> children;  // Directory only
    // File only.
    const ssd::BlockDevice* device = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

Node MakeDirectory(std::string name, std::vector<Node> children);
Node MakeFile(std::string name, const ssd::BlockDevice* device, std::uint64_t offset,
              std::uint64_t size);

/// A read-only view of a directory hierarchy.
///
/// Children are sorted by name on construction, so iteration order is
/// deterministic and lookup is a binary search.
class ReadOnlyTree {
public:
    ReadOnlyTree() = default;

    /// The tree keeps `devices` alive; `root` must be a directory whose file
    /// nodes point into them.
    ReadOnlyTree(std::vector<std::shared_ptr<const ssd::BlockDevice>> devices, Node root);

    /// A source whose whole contents are already in memory. Copies the bytes
    /// (one device per file), so this is for tests and small sources -- an
    /// image-backed tree built by an archive reader shares one device instead.
    struct MemoryFile {
        std::string path;  // may contain '/' to nest
        std::vector<std::uint8_t> bytes;
    };
    static std::optional<ReadOnlyTree> FromMemoryFiles(std::vector<MemoryFile> files,
                                                      std::string& error);

    /// An explicit file list over one shared device -- what an archive reader
    /// produces. No copying: each file is a range of `device`. `path` may
    /// contain '/' to nest.
    struct RangeFile {
        std::string path;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };
    static std::optional<ReadOnlyTree> FromRanges(std::shared_ptr<const ssd::BlockDevice> device,
                                                 std::vector<RangeFile> files,
                                                 std::string& error);

    /// Read a host directory (UTF-8 path) into a tree, recursively.
    ///
    /// A host directory has no single device behind it, so each file is read
    /// into memory. An image-backed tree built by an archive reader shares one
    /// device instead and copies nothing.
    static std::optional<ReadOnlyTree> FromHostDirectory(const std::string& path,
                                                        std::string& error);

    const Node& root() const noexcept { return root_; }
    bool empty() const noexcept { return root_.children.empty(); }

    const Node* Find(std::string_view path, TreeError* error = nullptr) const;
    bool Exists(std::string_view path) const;

    /// Direct child names of a directory, in sorted order. Empty (with
    /// NotADirectory) when the path is not a directory.
    std::vector<std::string> List(std::string_view path, TreeError* error = nullptr) const;

    /// Size of a file; nullopt (with NotAFile) for anything else.
    std::optional<std::uint64_t> Size(std::string_view path, TreeError* error = nullptr) const;

    /// Whole-file read.
    std::optional<std::vector<std::uint8_t>> Read(std::string_view path,
                                                 TreeError* error = nullptr) const;

    /// Partial read. A range that runs past the end of the file is refused
    /// (OutOfRange), never clamped -- a short read would silently look like
    /// truncated data.
    std::optional<std::vector<std::uint8_t>> ReadAt(std::string_view path, std::uint64_t offset,
                                                   std::size_t length,
                                                   TreeError* error = nullptr) const;

    /// Number of nodes including the root, for diagnostics and tests.
    std::size_t node_count() const noexcept;

    const std::vector<std::shared_ptr<const ssd::BlockDevice>>& devices() const noexcept {
        return devices_;
    }

private:
    std::vector<std::shared_ptr<const ssd::BlockDevice>> devices_;
    Node root_{Node::Kind::Directory, "", {}, nullptr, 0, 0};
};

}  // namespace zlong::mount
