// 烛龙 (ZhuLong) - archive structure parsing.
//
// Two shapes, one parser: a "flat" archive (one table of named entries, one
// string table, then the data) and a "partitioned" one (same shape, but each
// entry also carries a hash-region size and the entry is wider).
//
// ============================================================================
// These layouts are THIS PROJECT'S MODEL of the formats. Every field position
// lives in ArchiveLayout as data, and the parser below is written only in terms
// of that struct, so correcting a layout is an edit to the constants rather than
// a rewrite. The tests pin the model with independently written byte vectors --
// they prove the parser is self-consistent, not that it matches the real thing.
// ============================================================================
//
// Structural parsing only: an entry is a named byte range. What is inside it is
// the next layer's business.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zlong/ssd/block_device.h"

namespace zlong::mount {

/// Marks a field an archive layout does not have.
inline constexpr std::uint32_t kNoField = 0xFFFF'FFFFu;

/// Upper bound on entries, so a corrupt count cannot ask for a huge allocation
/// before any range check has run.
inline constexpr std::uint32_t kMaxArchiveEntries = 1u << 16;

/// Where an archive's fields live, relative to the archive's start.
struct ArchiveLayout {
    std::string_view magic;                  // exactly four characters
    std::uint32_t entry_count_offset = 0;
    std::uint32_t string_table_size_offset = 0;
    std::uint32_t entries_offset = 0;
    std::uint32_t entry_bytes = 0;
    std::uint32_t entry_offset_field = 0;
    std::uint32_t entry_size_field = 0;
    std::uint32_t entry_name_offset_field = 0;
    /// kNoField for the flat form.
    std::uint32_t entry_hashed_size_field = kNoField;
};

/// Header, one table of 0x18-byte entries, one string table, then the data.
inline constexpr ArchiveLayout kFlatArchive{"PFS0", 4, 8, 16, 0x18, 0, 8, 16, kNoField};

/// Same shape with the hashed-region size per entry and 0x40-byte entries.
inline constexpr ArchiveLayout kPartitionedArchive{"HFS0", 4, 8, 16, 0x40, 0, 8, 16, 20};

enum class ArchiveError {
    None,
    /// The archive's header does not fit in the device.
    DeviceTooSmall,
    BadMagic,
    TooManyEntries,
    /// The entry table or the string table runs past the end of the device.
    TableOutOfRange,
    /// An entry's name offset is outside the string table.
    NameOutOfRange,
    /// An entry's name is not terminated inside the string table.
    NameNotTerminated,
    EmptyName,
    /// An entry's data range is outside the device.
    DataOutOfRange,
    /// Two entries share a name.
    DuplicateName,
};

const char* ToString(ArchiveError error) noexcept;

struct ArchiveEntry {
    std::string name;
    /// Absolute offset in the backing device, already resolved against the data
    /// region -- callers never need to know where the tables ended.
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    /// The hash-region size of the partitioned form; always 0 for the flat form.
    std::uint64_t hashed_size = 0;
};

struct Archive {
    std::vector<ArchiveEntry> entries;
    /// Absolute offset of the data region.
    std::uint64_t data_base = 0;
    /// Absolute offset one past the archive's last byte.
    std::uint64_t end = 0;

    const ArchiveEntry* Find(std::string_view name) const;
};

std::optional<Archive> ParseArchive(const ssd::BlockDevice& device, std::uint64_t base,
                                    const ArchiveLayout& layout, ArchiveError* error = nullptr);

}  // namespace zlong::mount
