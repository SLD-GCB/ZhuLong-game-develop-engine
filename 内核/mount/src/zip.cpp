#include "zlong/mount/zip.h"

#include <set>

namespace zlong::mount {

namespace {

constexpr std::uint64_t kEndRecordBytes = 22;
/// A zip comment can be at most 64 KiB, so the end record starts within this
/// many bytes of the end.
constexpr std::uint64_t kMaxCommentBytes = 0xFFFF;
constexpr std::uint32_t kZip64Sentinel16 = 0xFFFF;
constexpr std::uint32_t kZip64Sentinel32 = 0xFFFF'FFFFu;

std::uint16_t ReadU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void SetError(ZipError* error, ZipError value) {
    if (error != nullptr) {
        *error = value;
    }
}

}  // namespace

const char* ToString(ZipError error) noexcept {
    switch (error) {
    case ZipError::None:
        return "none";
    case ZipError::NoEndRecord:
        return "no end-of-central-directory record";
    case ZipError::UnsupportedZip64:
        return "the archive is zip64, which is not implemented";
    case ZipError::TooManyEntries:
        return "the entry count is out of range";
    case ZipError::DirectoryOutOfRange:
        return "the central directory runs past the device";
    case ZipError::BadCentralSignature:
        return "a central directory entry has the wrong signature";
    case ZipError::BadLocalSignature:
        return "a local file header has the wrong signature";
    case ZipError::LocalHeaderOutOfRange:
        return "a local file header runs past the device";
    case ZipError::DataOutOfRange:
        return "an entry's data range is outside the device";
    case ZipError::EmptyName:
        return "an entry has an empty name";
    case ZipError::DuplicateName:
        return "two entries share a name";
    }
    return "unknown";
}

const ZipEntry* Zip::Find(std::string_view name) const {
    for (const ZipEntry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

bool LooksLikeZip(const ssd::BlockDevice& device) {
    std::uint8_t probe[4] = {};
    if (device.size() < sizeof(probe) || !device.ReadAt(0, probe, sizeof(probe))) {
        return false;
    }
    return ReadU32(probe) == kZipLocalSignature;
}

std::optional<Zip> ParseZip(const ssd::BlockDevice& device, ZipError* error) {
    const std::uint64_t size = device.size();
    if (size < kEndRecordBytes) {
        SetError(error, ZipError::NoEndRecord);
        return std::nullopt;
    }

    // The end record sits at the very end, or before a trailing comment. Scan
    // backwards for its signature rather than trusting a length in front of it.
    const std::uint64_t window = size < (kEndRecordBytes + kMaxCommentBytes)
                                     ? size
                                     : (kEndRecordBytes + kMaxCommentBytes);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(window));
    if (!device.ReadAt(size - window, tail.data(), tail.size())) {
        SetError(error, ZipError::NoEndRecord);
        return std::nullopt;
    }

    std::uint64_t end_offset = 0;
    bool found = false;
    for (std::uint64_t back = 0; back + kEndRecordBytes <= window; ++back) {
        const std::uint64_t candidate = window - kEndRecordBytes - back;
        if (ReadU32(tail.data() + candidate) == kZipEndSignature) {
            end_offset = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        SetError(error, ZipError::NoEndRecord);
        return std::nullopt;
    }

    const std::uint8_t* const end = tail.data() + end_offset;
    const std::uint32_t entry_count = ReadU16(end + 10);
    const std::uint32_t directory_size = ReadU32(end + 12);
    const std::uint32_t directory_offset = ReadU32(end + 16);

    if (entry_count == kZip64Sentinel16 || directory_size == kZip64Sentinel32 ||
        directory_offset == kZip64Sentinel32) {
        SetError(error, ZipError::UnsupportedZip64);
        return std::nullopt;
    }
    if (entry_count > kMaxZipEntries) {
        SetError(error, ZipError::TooManyEntries);
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(directory_offset) + directory_size > size) {
        SetError(error, ZipError::DirectoryOutOfRange);
        return std::nullopt;
    }

    std::vector<std::uint8_t> directory(directory_size);
    if (directory_size != 0 &&
        !device.ReadAt(directory_offset, directory.data(), directory.size())) {
        SetError(error, ZipError::DirectoryOutOfRange);
        return std::nullopt;
    }

    Zip zip;
    zip.entries.reserve(entry_count);
    std::set<std::string> seen;

    std::uint64_t cursor = 0;
    for (std::uint32_t index = 0; index < entry_count; ++index) {
        if (cursor + kZipCentralHeaderBytes > directory_size) {
            SetError(error, ZipError::DirectoryOutOfRange);
            return std::nullopt;
        }
        const std::uint8_t* const header = directory.data() + cursor;
        if (ReadU32(header) != kZipCentralSignature) {
            SetError(error, ZipError::BadCentralSignature);
            return std::nullopt;
        }

        const std::uint16_t method = ReadU16(header + 10);
        const std::uint32_t compressed_size = ReadU32(header + 20);
        const std::uint32_t uncompressed_size = ReadU32(header + 24);
        const std::uint16_t name_length = ReadU16(header + 28);
        const std::uint16_t extra_length = ReadU16(header + 30);
        const std::uint16_t comment_length = ReadU16(header + 32);
        const std::uint32_t local_offset = ReadU32(header + 42);

        const std::uint64_t record_bytes =
            kZipCentralHeaderBytes + name_length + extra_length + comment_length;
        if (cursor + record_bytes > directory_size) {
            SetError(error, ZipError::DirectoryOutOfRange);
            return std::nullopt;
        }
        if (compressed_size == kZip64Sentinel32 || uncompressed_size == kZip64Sentinel32 ||
            local_offset == kZip64Sentinel32) {
            SetError(error, ZipError::UnsupportedZip64);
            return std::nullopt;
        }

        ZipEntry entry;
        entry.method = method;
        entry.size = uncompressed_size;
        entry.compressed_size = compressed_size;
        entry.name.assign(reinterpret_cast<const char*>(header + kZipCentralHeaderBytes),
                          name_length);
        if (entry.name.empty()) {
            SetError(error, ZipError::EmptyName);
            return std::nullopt;
        }
        if (!seen.insert(entry.name).second) {
            SetError(error, ZipError::DuplicateName);
            return std::nullopt;
        }

        // The data offset needs the *local* header's name and extra lengths:
        // they are allowed to differ from the central directory's.
        std::uint8_t local[kZipLocalHeaderBytes] = {};
        if (local_offset > size || kZipLocalHeaderBytes > size - local_offset ||
            !device.ReadAt(local_offset, local, sizeof(local))) {
            SetError(error, ZipError::LocalHeaderOutOfRange);
            return std::nullopt;
        }
        if (ReadU32(local) != kZipLocalSignature) {
            SetError(error, ZipError::BadLocalSignature);
            return std::nullopt;
        }
        const std::uint16_t local_name_length = ReadU16(local + 26);
        const std::uint16_t local_extra_length = ReadU16(local + 28);
        entry.data_offset = static_cast<std::uint64_t>(local_offset) + kZipLocalHeaderBytes +
                            local_name_length + local_extra_length;

        if (entry.data_offset > size || compressed_size > size - entry.data_offset) {
            SetError(error, ZipError::DataOutOfRange);
            return std::nullopt;
        }

        zip.entries.push_back(std::move(entry));
        cursor += record_bytes;
    }

    SetError(error, ZipError::None);
    return zip;
}

std::optional<std::vector<ReadOnlyTree::RangeFile>> ZipToRanges(const Zip& zip,
                                                               std::string& error) {
    std::vector<ReadOnlyTree::RangeFile> files;
    files.reserve(zip.entries.size());
    for (const ZipEntry& entry : zip.entries) {
        if (!entry.stored()) {
            // Name the entry and the method: a mount that silently omitted this
            // file would look complete and be wrong.
            error = "the zip entry \"" + entry.name + "\" uses compression method " +
                    std::to_string(entry.method) +
                    ", which this reader does not implement (only stored entries)";
            return std::nullopt;
        }
        if (entry.compressed_size != entry.size) {
            error = "the zip entry \"" + entry.name +
                    "\" is stored but its two sizes disagree";
            return std::nullopt;
        }
        ReadOnlyTree::RangeFile file;
        file.path = entry.name;
        file.offset = entry.data_offset;
        file.size = entry.size;
        files.push_back(std::move(file));
    }
    error.clear();
    return files;
}

}  // namespace zlong::mount
