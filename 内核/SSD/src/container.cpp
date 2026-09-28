#include "zlong/ssd/container.h"

#include <cstring>

namespace zlong::ssd {

namespace {

constexpr char kMagic[8] = {'Z', 'L', 'C', 'O', 'N', 'T', '0', '1'};

std::uint32_t ReadU32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return value;
}

}  // namespace

std::optional<Container> Container::Open(const BlockDevice& device, ContainerError* error) {
    const auto fail = [error](ContainerError code) -> std::optional<Container> {
        if (error != nullptr) {
            *error = code;
        }
        return std::nullopt;
    };

    if (device.size() < kContainerHeaderSize) {
        return fail(ContainerError::DeviceTooSmall);
    }

    std::uint8_t header[kContainerHeaderSize] = {};
    if (!device.ReadAt(0, header, sizeof(header))) {
        return fail(ContainerError::DeviceTooSmall);
    }
    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
        return fail(ContainerError::BadMagic);
    }

    const std::uint32_t version = ReadU32(header + 8);
    const std::uint32_t count = ReadU32(header + 12);
    const std::uint64_t table_offset = ReadU64(header + 16);

    if (version != kContainerVersion) {
        return fail(ContainerError::UnsupportedVersion);
    }
    if (count > kMaxPartitions) {
        return fail(ContainerError::TooManyPartitions);
    }

    const std::uint64_t table_bytes = static_cast<std::uint64_t>(count) * kPartitionEntrySize;
    if (table_offset > device.size() || table_bytes > device.size() - table_offset) {
        return fail(ContainerError::TableOutOfRange);
    }

    std::vector<std::uint8_t> table(static_cast<std::size_t>(table_bytes), 0);
    if (table_bytes != 0 && !device.ReadAt(table_offset, table.data(), table.size())) {
        return fail(ContainerError::TableOutOfRange);
    }

    Container container(&device);
    container.partitions_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* entry = table.data() + static_cast<std::size_t>(i) * kPartitionEntrySize;
        PartitionInfo info;
        info.id = ReadU64(entry);
        info.offset = ReadU64(entry + 8);
        info.length = ReadU64(entry + 16);
        info.kind = ReadU32(entry + 24);
        info.flags = ReadU32(entry + 28);

        if (info.offset > device.size() || info.length > device.size() - info.offset) {
            return fail(ContainerError::PartitionOutOfRange);
        }
        container.partitions_.push_back(info);
    }

    if (error != nullptr) {
        *error = ContainerError::None;
    }
    return container;
}

std::optional<Partition> Container::OpenPartition(std::size_t index) const {
    if (index >= partitions_.size()) {
        return std::nullopt;
    }
    return Partition(*device_, partitions_[index]);
}

std::optional<Partition> Container::FindPartitionById(std::uint64_t id) const {
    for (const auto& info : partitions_) {
        if (info.id == id) {
            return Partition(*device_, info);
        }
    }
    return std::nullopt;
}

bool Partition::ReadAt(std::uint64_t offset, void* dst, std::size_t n) const {
    if (device_ == nullptr || offset > info_.length || n > info_.length - offset) {
        return false;
    }
    return device_->ReadAt(info_.offset + offset, dst, n);
}

std::vector<std::uint8_t> Partition::Read(std::uint64_t offset, std::size_t n) const {
    std::vector<std::uint8_t> buffer(n, 0);
    if (n == 0 || !ReadAt(offset, buffer.data(), n)) {
        buffer.clear();
    }
    return buffer;
}

}  // namespace zlong::ssd
