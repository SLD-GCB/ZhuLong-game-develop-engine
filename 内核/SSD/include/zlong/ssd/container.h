// 烛龙 (ZhuLong) - game container structure: a fixed header followed by a
// partition table.
//
// Structural parsing only: container and partition geometry. Partition payloads
// are opaque byte ranges here; interpreting them (filesystems) is the next
// layer up.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "zlong/ssd/block_device.h"

namespace zlong::ssd {

/// On-disk layout (all little-endian):
///
///   offset 0   char     magic[8]      "ZLCONT01"
///   offset 8   uint32   version       currently 1
///   offset 12  uint32   partition_count
///   offset 16  uint64   table_offset  absolute offset of the partition table
///   offset 24  uint64   reserved      must be 0
///
/// followed at `table_offset` by `partition_count` entries of:
///
///   offset 0   uint64   id
///   offset 8   uint64   offset        absolute offset of the partition payload
///   offset 16  uint64   length        payload length in bytes
///   offset 24  uint32   kind
///   offset 28  uint32   flags
inline constexpr std::size_t kContainerHeaderSize = 32;
inline constexpr std::size_t kPartitionEntrySize = 32;
inline constexpr std::uint32_t kContainerVersion = 1;
inline constexpr std::uint32_t kMaxPartitions = 64;

struct PartitionInfo {
    std::uint64_t id = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t kind = 0;
    std::uint32_t flags = 0;
};

enum class ContainerError {
    None,
    DeviceTooSmall,
    BadMagic,
    UnsupportedVersion,
    TableOutOfRange,
    PartitionOutOfRange,
    TooManyPartitions,
};

/// A byte range inside a container. Borrows the device: it must outlive this.
class Partition {
public:
    Partition() = default;
    Partition(const BlockDevice& device, PartitionInfo info)
        : device_(&device), info_(info) {}

    const PartitionInfo& info() const noexcept { return info_; }
    std::uint64_t size() const noexcept { return info_.length; }

    bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const;
    std::vector<std::uint8_t> Read(std::uint64_t offset, std::size_t n) const;

private:
    const BlockDevice* device_ = nullptr;
    PartitionInfo info_{};
};

class Container {
public:
    /// Parse the header and partition table. On failure returns nullopt and,
    /// when `error` is given, why.
    static std::optional<Container> Open(const BlockDevice& device,
                                         ContainerError* error = nullptr);

    std::size_t partition_count() const noexcept { return partitions_.size(); }
    const std::vector<PartitionInfo>& partitions() const noexcept { return partitions_; }
    const PartitionInfo& partition(std::size_t index) const { return partitions_.at(index); }

    /// The partition at `index`, or nullopt when the index is out of range.
    std::optional<Partition> OpenPartition(std::size_t index) const;

    /// First partition with the given id, or nullopt.
    std::optional<Partition> FindPartitionById(std::uint64_t id) const;

private:
    explicit Container(const BlockDevice* device) : device_(device) {}

    const BlockDevice* device_ = nullptr;
    std::vector<PartitionInfo> partitions_;
};

}  // namespace zlong::ssd
