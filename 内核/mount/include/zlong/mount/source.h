// 烛龙 (ZhuLong) - opening a source: a host directory, or a host image file.
//
// Host paths are UTF-8. ssd::FileBlockDevice opens with a narrow fstream path,
// which on Windows goes through the ANSI code page, so a path with non-ASCII
// characters fails there; HostFileDevice below carries a
// std::filesystem::path instead, which converts UTF-8 correctly.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zlong/ssd/block_device.h"

namespace zlong::mount {

enum class SourceError {
    None,
    EmptyPath,
    NotFound,
    NotAFile,
    NotADirectory,
    /// The host refused to open or list the path.
    HostError,
    /// The opened file could not be read.
    DeviceError,
};

const char* ToString(SourceError error) noexcept;

/// A host file as a byte-addressable, **read-only** device.
class HostFileDevice final : public ssd::BlockDevice {
public:
    HostFileDevice() = default;
    HostFileDevice(HostFileDevice&& other) noexcept;
    HostFileDevice& operator=(HostFileDevice&& other) noexcept;
    ~HostFileDevice() override;

    HostFileDevice(const HostFileDevice&) = delete;
    HostFileDevice& operator=(const HostFileDevice&) = delete;

    static std::optional<HostFileDevice> Open(const std::string& utf8_path);

    std::uint64_t size() const override { return size_; }
    bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const override;
    /// Always false: a source is opened for reading.
    bool WriteAt(std::uint64_t offset, const void* src, std::size_t n) override;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::uint64_t size_ = 0;
};

/// A host image file as a shared device, ready to hand to an archive reader.
std::optional<std::shared_ptr<const ssd::BlockDevice>> OpenHostImage(
    const std::string& utf8_path, SourceError* error = nullptr);

/// Whole-file read, for a source that is a small file rather than an image.
std::optional<std::vector<std::uint8_t>> ReadHostFile(const std::string& utf8_path,
                                                      SourceError* error = nullptr);

/// Whether the path is a directory. Used to tell the two source forms apart --
/// the choice is structural, never guessed from a filename.
bool IsHostDirectory(const std::string& utf8_path, SourceError* error = nullptr);

}  // namespace zlong::mount
