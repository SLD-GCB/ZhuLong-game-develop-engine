# 内核-ssd

# `block_device.h`

`内核/SSD/include/zlong/ssd/block_device.h`

```
烛龙 (ZhuLong) - byte-addressable block device.

Everything above this (container parsing, filesystems, the guest-facing
storage device) talks to a BlockDevice, so the same code serves a host file
and the emulated SD card.
```

```cpp
inline constexpr std::uint32_t kDefaultSectorSize = 512;
class BlockDevice {
    virtual ~BlockDevice() = default;
    // Total size in bytes.
    virtual std::uint64_t size() const = 0;
    // Logical sector size. 512 unless a device says otherwise.
    virtual std::uint32_t sector_size() const noexcept { return kDefaultSectorSize; }
    // Byte-granular access. Return false for any range outside the device;
    // never throw. Reads past the end must not be partially applied.
    virtual bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const = 0;
    virtual bool WriteAt(std::uint64_t offset, const void* src, std::size_t n) = 0;
    bool ReadSectors(std::uint64_t lba, void* dst, std::uint32_t count) const;
    bool WriteSectors(std::uint64_t lba, const void* src, std::uint32_t count);
// RAM-backed device. Used by tests and by the ("disk" in memory) case.
class MemoryBlockDevice final : public BlockDevice {
    explicit MemoryBlockDevice(std::uint64_t size)
        : data_(static_cast<std::size_t>(size), 0) {}
    explicit MemoryBlockDevice(std::vector<std::uint8_t> data) : data_(std::move(data)) {}
    bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const override;
    bool WriteAt(std::uint64_t offset, const void* src, std::size_t n) override;
    const std::vector<std::uint8_t>& bytes() const noexcept { return data_; }
// Host-file-backed device (read/write, opened binary).
class FileBlockDevice final : public BlockDevice {
    // Opens an existing file, or creates a zero-filled one of `create_size`
    // bytes when `create` is true.
    static bool Open(const char* path, FileBlockDevice& out, bool create = false,
                     std::uint64_t create_size = 0);
    bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const override;
    bool WriteAt(std::uint64_t offset, const void* src, std::size_t n) override;
    // Opaque so <fstream> stays out of this header.
    struct Impl;
```

---

# `container.h`

`内核/SSD/include/zlong/ssd/container.h`

```
烛龙 (ZhuLong) - game container structure: a fixed header followed by a
partition table.

Structural parsing only: container and partition geometry. Partition payloads
are opaque byte ranges here; interpreting them (filesystems) is the next
layer up.
```

```cpp
// On-disk layout (all little-endian):
//
// offset 0   char     magic[8]      "ZLCONT01"
// offset 8   uint32   version       currently 1
// offset 12  uint32   partition_count
// offset 16  uint64   table_offset  absolute offset of the partition table
// offset 24  uint64   reserved      must be 0
//
// followed at `table_offset` by `partition_count` entries of:
//
// offset 0   uint64   id
// offset 8   uint64   offset        absolute offset of the partition payload
// offset 16  uint64   length        payload length in bytes
// offset 24  uint32   kind
// offset 28  uint32   flags
inline constexpr std::size_t kContainerHeaderSize = 32;
inline constexpr std::size_t kPartitionEntrySize = 32;
inline constexpr std::uint32_t kContainerVersion = 1;
inline constexpr std::uint32_t kMaxPartitions = 64;
struct PartitionInfo {
enum class ContainerError {
// A byte range inside a container. Borrows the device: it must outlive this.
class Partition {
    const PartitionInfo& info() const noexcept { return info_; }
    bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const;
class Container {
    // Parse the header and partition table. On failure returns nullopt and,
    // when `error` is given, why.
    static std::optional<Container> Open(const BlockDevice& device,
                                         ContainerError* error = nullptr);
    const std::vector<PartitionInfo>& partitions() const noexcept { return partitions_; }
    const PartitionInfo& partition(std::size_t index) const { return partitions_.at(index); }
    explicit Container(const BlockDevice* device) : device_(device) {}
```

---
