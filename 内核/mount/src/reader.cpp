#include "zlong/mount/reader.h"

#include <cstring>
#include <utility>

#include "zlong/mount/archive.h"

namespace zlong::mount {

namespace {

/// Cheap magic probe: read the four bytes at the partition's start and compare.
bool StartsWith(const ssd::BlockDevice& device, const ssd::PartitionInfo& info,
                std::string_view magic) {
    if (magic.size() != 4 || info.length < 4) {
        return false;
    }
    std::uint8_t probe[4] = {};
    if (!device.ReadAt(info.offset, probe, sizeof(probe))) {
        return false;
    }
    return std::memcmp(probe, magic.data(), 4) == 0;
}

/// Flatten a subtree into range files, refusing any file that does not borrow
/// `device` -- a list meant for one device must not silently mix two.
bool FlattenNode(const Node& node, const ssd::BlockDevice& device, const std::string& prefix,
                 std::vector<ReadOnlyTree::RangeFile>& out, std::string& error) {
    for (const Node& child : node.children) {
        const std::string path = prefix.empty() ? child.name : prefix + "/" + child.name;
        if (child.kind == Node::Kind::Directory) {
            if (!FlattenNode(child, device, path, out, error)) {
                return false;
            }
            continue;
        }
        if (child.device != &device) {
            error = "a file in the expansion does not borrow the source device";
            return false;
        }
        ReadOnlyTree::RangeFile file;
        file.path = path;
        file.offset = child.offset;
        file.size = child.size;
        out.push_back(std::move(file));
    }
    return true;
}

}  // namespace

bool OpaquePartition::Matches(const ssd::BlockDevice&, const ssd::PartitionInfo&) const {
    return true;
}

std::optional<ReadOnlyTree> OpaquePartition::Expand(std::shared_ptr<const ssd::BlockDevice> device,
                                                    const ssd::PartitionInfo& info,
                                                    std::string& error) const {
    if (device == nullptr) {
        error = "the partition has no backing device";
        return std::nullopt;
    }
    if (info.offset > device->size() || info.length > device->size() - info.offset) {
        error = "the partition range is outside the device";
        return std::nullopt;
    }

    std::vector<ReadOnlyTree::RangeFile> files;
    ReadOnlyTree::RangeFile file;
    file.path = std::to_string(info.id) + ".bin";
    file.offset = info.offset;
    file.size = info.length;
    files.push_back(std::move(file));
    return ReadOnlyTree::FromRanges(std::move(device), std::move(files), error);
}

bool FlatArchivePartition::Matches(const ssd::BlockDevice& device,
                                   const ssd::PartitionInfo& info) const {
    return StartsWith(device, info, kFlatArchive.magic);
}

std::optional<ReadOnlyTree> FlatArchivePartition::Expand(
    std::shared_ptr<const ssd::BlockDevice> device, const ssd::PartitionInfo& info,
    std::string& error) const {
    if (device == nullptr) {
        error = "the partition has no backing device";
        return std::nullopt;
    }

    ArchiveError archive_error = ArchiveError::None;
    const auto archive = ParseArchive(*device, info.offset, kFlatArchive, &archive_error);
    if (!archive.has_value()) {
        error = std::string("the partition is not a readable flat archive: ") +
                ToString(archive_error);
        return std::nullopt;
    }
    // The archive must fit inside the partition, not merely inside the device.
    const std::uint64_t partition_end = info.offset + info.length;
    if (archive->end > partition_end) {
        error = "the archive runs past the end of the partition";
        return std::nullopt;
    }

    std::vector<ReadOnlyTree::RangeFile> files;
    files.reserve(archive->entries.size());
    for (const ArchiveEntry& entry : archive->entries) {
        ReadOnlyTree::RangeFile file;
        file.path = entry.name;
        file.offset = entry.offset;
        file.size = entry.size;
        files.push_back(std::move(file));
    }
    return ReadOnlyTree::FromRanges(std::move(device), std::move(files), error);
}

void PartitionReaders::Add(std::shared_ptr<const PartitionReader> reader) {
    if (reader != nullptr) {
        readers_.push_back(std::move(reader));
    }
}

std::vector<std::string> PartitionReaders::names() const {
    std::vector<std::string> names;
    names.reserve(readers_.size());
    for (const auto& reader : readers_) {
        names.push_back(reader->name());
    }
    return names;
}

const PartitionReader* PartitionReaders::Choose(const ssd::BlockDevice& device,
                                                const ssd::PartitionInfo& info) const {
    for (const auto& reader : readers_) {
        if (reader->Matches(device, info)) {
            return reader.get();
        }
    }
    return nullptr;
}

std::optional<ReadOnlyTree> PartitionReaders::Expand(std::shared_ptr<const ssd::BlockDevice> device,
                                                     const ssd::PartitionInfo& info,
                                                     std::string& error) const {
    if (device == nullptr) {
        error = "the partition has no backing device";
        return std::nullopt;
    }
    for (const auto& reader : readers_) {
        if (!reader->Matches(*device, info)) {
            continue;
        }
        // A matching reader owns the outcome: if it fails, say so with its name
        // rather than falling through to the floor and quietly changing shape.
        auto tree = reader->Expand(std::move(device), info, error);
        if (!tree.has_value() && error.find(reader->name()) == std::string::npos) {
            error = std::string(reader->name()) + ": " + error;
        }
        return tree;
    }
    error = "no reader claims this partition";
    return std::nullopt;
}

PartitionReaders PartitionReaders::Default() {
    PartitionReaders readers;
    readers.Add(std::make_shared<FlatArchivePartition>());
    readers.Add(std::make_shared<OpaquePartition>());
    return readers;
}

std::optional<std::vector<ReadOnlyTree::RangeFile>> ExpandDeviceToFiles(
    const std::shared_ptr<const ssd::BlockDevice>& device, const PartitionReaders& readers,
    std::string& error) {
    if (device == nullptr) {
        error = "the source has no device";
        return std::nullopt;
    }

    std::vector<ssd::PartitionInfo> partitions;
    bool from_container = false;
    ssd::ContainerError container_error = ssd::ContainerError::None;
    const auto container = ssd::Container::Open(*device, &container_error);
    if (container.has_value()) {
        partitions = container->partitions();
        from_container = true;
    } else {
        // Not a container we understand: treat the whole device as one
        // partition. That is the common case for a plain image.
        ssd::PartitionInfo whole;
        whole.id = 0;
        whole.offset = 0;
        whole.length = device->size();
        partitions.push_back(whole);
    }
    if (partitions.empty()) {
        error = "the container declares no partitions";
        return std::nullopt;
    }

    const bool prefix_partitions = partitions.size() > 1;
    std::vector<ReadOnlyTree::RangeFile> files;
    for (const ssd::PartitionInfo& info : partitions) {
        const PartitionReader* chosen = readers.Choose(*device, info);
        if (chosen == nullptr) {
            error = "partition " + std::to_string(info.id) + ": no reader claims it";
            return std::nullopt;
        }
        if (!from_container && chosen->is_floor()) {
            // A bare device that nothing recognises would otherwise "mount" as
            // one opaque file, which looks like success and is useless.
            error = "the source format is not recognised: it is not a container, "
                    "and no reader claims the whole device";
            return std::nullopt;
        }
        std::string why;
        const auto tree = readers.Expand(device, info, why);
        if (!tree.has_value()) {
            error = "partition " + std::to_string(info.id) + ": " + why;
            return std::nullopt;
        }
        const std::string prefix = prefix_partitions ? std::to_string(info.id) : std::string();
        if (!FlattenNode(tree->root(), *device, prefix, files, error)) {
            return std::nullopt;
        }
    }
    return files;
}

}  // namespace zlong::mount
