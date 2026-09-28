// 烛龙 (ZhuLong) - ZIP reading, for a source shipped as a zip.
//
// Structural parsing only: the central directory is parsed to a list of named
// byte ranges, and a stored entry is a range of the zip itself -- nothing is
// copied. An entry that is actually compressed is refused by name and method
// rather than skipped, because a silently missing entry is worse than an error.
//
// ============================================================================
// SCOPE: this parses the central directory and the local headers. Stored entries
// are supported (which is what a firmware package uses -- its payload is already
// incompressible). A deflated entry is a named error, not a wrong answer; adding
// inflate later is a change to ZipToRanges and nothing else.
// ============================================================================

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zlong/mount/tree.h"
#include "zlong/ssd/block_device.h"

namespace zlong::mount {

/// The local file header is 30 bytes, before the name and extra fields.
inline constexpr std::size_t kZipLocalHeaderBytes = 30;
/// The central directory header is 46 bytes, before the name and extra fields.
inline constexpr std::size_t kZipCentralHeaderBytes = 46;
inline constexpr std::uint32_t kZipLocalSignature = 0x04034b50;
inline constexpr std::uint32_t kZipCentralSignature = 0x02014b50;
inline constexpr std::uint32_t kZipEndSignature = 0x06054b50;
inline constexpr std::uint16_t kZipMethodStored = 0;
inline constexpr std::uint16_t kZipMethodDeflate = 8;
/// Upper bound on entries, so a corrupt count cannot ask for a huge allocation.
inline constexpr std::uint32_t kMaxZipEntries = 1u << 16;

enum class ZipError {
    None,
    /// No end-of-central-directory record in the last 64 KiB + 22 bytes.
    NoEndRecord,
    /// The archive declares zip64, which this reader does not implement.
    UnsupportedZip64,
    TooManyEntries,
    /// The central directory runs past the end of the device.
    DirectoryOutOfRange,
    BadCentralSignature,
    BadLocalSignature,
    LocalHeaderOutOfRange,
    DataOutOfRange,
    EmptyName,
    DuplicateName,
};

const char* ToString(ZipError error) noexcept;

struct ZipEntry {
    std::string name;
    /// 0 = stored, 8 = deflate. Anything else is left to the caller to refuse.
    std::uint16_t method = 0;
    std::uint64_t size = 0;            // uncompressed size
    std::uint64_t compressed_size = 0;
    /// Absolute offset in the device of the entry's data.
    std::uint64_t data_offset = 0;

    bool stored() const noexcept { return method == kZipMethodStored; }
};

struct Zip {
    std::vector<ZipEntry> entries;

    const ZipEntry* Find(std::string_view name) const;
};

/// Whether the device starts with a local file header. Cheap enough to use as a
/// source-form probe.
bool LooksLikeZip(const ssd::BlockDevice& device);

std::optional<Zip> ParseZip(const ssd::BlockDevice& device, ZipError* error = nullptr);

/// The zip's entries as a file list over the same device. Refuses the whole list
/// if any entry is compressed, naming that entry -- so a partial mount cannot
/// look complete.
std::optional<std::vector<ReadOnlyTree::RangeFile>> ZipToRanges(const Zip& zip,
                                                               std::string& error);

}  // namespace zlong::mount
