#include "zlong/mount/archive.h"

#include <cstring>
#include <set>

namespace zlong::mount {

namespace {

std::uint32_t ReadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(p[index]) << (8 * index);
    }
    return value;
}

void SetError(ArchiveError* error, ArchiveError value) {
    if (error != nullptr) {
        *error = value;
    }
}

}  // namespace

const char* ToString(ArchiveError error) noexcept {
    switch (error) {
    case ArchiveError::None:
        return "none";
    case ArchiveError::DeviceTooSmall:
        return "the archive header does not fit in the device";
    case ArchiveError::BadMagic:
        return "the magic does not match the expected layout";
    case ArchiveError::TooManyEntries:
        return "the entry count is out of range";
    case ArchiveError::TableOutOfRange:
        return "the entry table or string table runs past the device";
    case ArchiveError::NameOutOfRange:
        return "an entry name offset is outside the string table";
    case ArchiveError::NameNotTerminated:
        return "an entry name is not terminated inside the string table";
    case ArchiveError::EmptyName:
        return "an entry has an empty name";
    case ArchiveError::DataOutOfRange:
        return "an entry's data range is outside the device";
    case ArchiveError::DuplicateName:
        return "two entries share a name";
    }
    return "unknown";
}

const ArchiveEntry* Archive::Find(std::string_view name) const {
    for (const ArchiveEntry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

std::optional<Archive> ParseArchive(const ssd::BlockDevice& device, std::uint64_t base,
                                    const ArchiveLayout& layout, ArchiveError* error) {
    if (layout.magic.size() != 4) {
        SetError(error, ArchiveError::BadMagic);
        return std::nullopt;
    }

    std::uint8_t header[16] = {};
    if (!device.ReadAt(base, header, sizeof(header))) {
        SetError(error, ArchiveError::DeviceTooSmall);
        return std::nullopt;
    }
    if (std::memcmp(header, layout.magic.data(), 4) != 0) {
        SetError(error, ArchiveError::BadMagic);
        return std::nullopt;
    }

    const std::uint32_t count = ReadU32(header + layout.entry_count_offset);
    const std::uint32_t string_table_size = ReadU32(header + layout.string_table_size_offset);
    if (count > kMaxArchiveEntries) {
        SetError(error, ArchiveError::TooManyEntries);
        return std::nullopt;
    }

    // Everything up to the end of the string table, read in one go: one bounded
    // allocation, and every later check is against a known-good buffer.
    const std::uint64_t table_bytes = static_cast<std::uint64_t>(count) * layout.entry_bytes;
    const std::uint64_t blob_bytes =
        static_cast<std::uint64_t>(layout.entries_offset) + table_bytes + string_table_size;
    if (base > device.size() || blob_bytes > device.size() - base) {
        SetError(error, ArchiveError::TableOutOfRange);
        return std::nullopt;
    }

    std::vector<std::uint8_t> blob(static_cast<std::size_t>(blob_bytes));
    if (blob_bytes != 0 && !device.ReadAt(base, blob.data(), blob.size())) {
        SetError(error, ArchiveError::TableOutOfRange);
        return std::nullopt;
    }

    const std::uint64_t data_base = base + blob_bytes;
    const std::uint8_t* const string_table =
        blob.data() + layout.entries_offset + table_bytes;

    Archive archive;
    archive.data_base = data_base;
    archive.entries.reserve(count);
    std::set<std::string> seen_names;

    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint8_t* const entry =
            blob.data() + layout.entries_offset + static_cast<std::uint64_t>(index) * layout.entry_bytes;

        ArchiveEntry parsed;
        parsed.offset = ReadU64(entry + layout.entry_offset_field);
        parsed.size = ReadU64(entry + layout.entry_size_field);
        const std::uint32_t name_offset = ReadU32(entry + layout.entry_name_offset_field);
        if (layout.entry_hashed_size_field != kNoField) {
            parsed.hashed_size = ReadU32(entry + layout.entry_hashed_size_field);
        }

        if (name_offset >= string_table_size) {
            SetError(error, ArchiveError::NameOutOfRange);
            return std::nullopt;
        }
        const char* const name_start = reinterpret_cast<const char*>(string_table) + name_offset;
        const std::size_t name_room = string_table_size - name_offset;
        const void* const terminator = std::memchr(name_start, '\0', name_room);
        if (terminator == nullptr) {
            SetError(error, ArchiveError::NameNotTerminated);
            return std::nullopt;
        }
        parsed.name.assign(name_start, static_cast<const char*>(terminator) - name_start);
        if (parsed.name.empty()) {
            SetError(error, ArchiveError::EmptyName);
            return std::nullopt;
        }
        if (!seen_names.insert(parsed.name).second) {
            SetError(error, ArchiveError::DuplicateName);
            return std::nullopt;
        }

        // Resolve the data range now, so no caller has to add the data base.
        if (data_base > device.size() || parsed.offset > device.size() - data_base ||
            parsed.size > device.size() - data_base - parsed.offset) {
            SetError(error, ArchiveError::DataOutOfRange);
            return std::nullopt;
        }
        parsed.offset += data_base;

        archive.entries.push_back(std::move(parsed));
    }

    archive.end = data_base;
    for (const ArchiveEntry& entry : archive.entries) {
        archive.end = std::max(archive.end, entry.offset + entry.size);
    }

    SetError(error, ArchiveError::None);
    return archive;
}

}  // namespace zlong::mount
