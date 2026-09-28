// 烛龙 (ZhuLong) - byte-addressable block device.
//
// Everything above this (container parsing, filesystems, the guest-facing
// storage device) talks to a BlockDevice, so the same code serves a host file
// and the emulated SD card.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zlong::ssd {

inline constexpr std::uint32_t kDefaultSectorSize = 512;

class BlockDevice {
public:
    virtual ~BlockDevice() = default;

    /// Total size in bytes.
    virtual std::uint64_t size() const = 0;

    /// Logical sector size. 512 unless a device says otherwise.
    virtual std::uint32_t sector_size() const noexcept { return kDefaultSectorSize; }

    /// Byte-granular access. Return false for any range outside the device;
    /// never throw. Reads past the end must not be partially applied.
    virtual bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const = 0;
    virtual bool WriteAt(std::uint64_t offset, const void* src, std::size_t n) = 0;

    std::uint64_t sector_count() const noexcept;
    bool ReadSectors(std::uint64_t lba, void* dst, std::uint32_t count) const;
    bool WriteSectors(std::uint64_t lba, const void* src, std::uint32_t count);
};

/// RAM-backed device. Used by tests and by the ("disk" in memory) case.
class MemoryBlockDevice final : public BlockDevice {
public:
    explicit MemoryBlockDevice(std::uint64_t size)
        : data_(static_cast<std::size_t>(size), 0) {}
    explicit MemoryBlockDevice(std::vector<std::uint8_t> data) : data_(std::move(data)) {}

    std::uint64_t size() const override { return data_.size(); }

    bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const override;
    bool WriteAt(std::uint64_t offset, const void* src, std::size_t n) override;

    std::vector<std::uint8_t>& bytes() noexcept { return data_; }
    const std::vector<std::uint8_t>& bytes() const noexcept { return data_; }

private:
    std::vector<std::uint8_t> data_;
};

/// Host-file-backed device (read/write, opened binary).
class FileBlockDevice final : public BlockDevice {
public:
    FileBlockDevice() = default;
    FileBlockDevice(FileBlockDevice&&) noexcept = default;
    FileBlockDevice& operator=(FileBlockDevice&&) noexcept = default;
    ~FileBlockDevice() override;

    /// Opens an existing file, or creates a zero-filled one of `create_size`
    /// bytes when `create` is true.
    static bool Open(const char* path, FileBlockDevice& out, bool create = false,
                     std::uint64_t create_size = 0);

    std::uint64_t size() const override { return size_; }
    bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const override;
    bool WriteAt(std::uint64_t offset, const void* src, std::size_t n) override;

private:
    // Opaque so <fstream> stays out of this header.
    struct Impl;
    Impl* impl_ = nullptr;
    std::uint64_t size_ = 0;
};

}  // namespace zlong::ssd
