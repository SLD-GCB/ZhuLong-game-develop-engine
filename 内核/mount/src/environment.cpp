#include "zlong/mount/environment.h"

#include <utility>
#include <vector>

#include "zlong/mount/reader.h"
#include "zlong/mount/source.h"
#include "zlong/mount/tree.h"
#include "zlong/mount/zip.h"

namespace zlong::mount {

namespace {

void SetError(EnvironmentError* error, std::string* detail, EnvironmentError value,
              std::string why) {
    if (error != nullptr) {
        *error = value;
    }
    if (detail != nullptr) {
        *detail = std::move(why);
    }
}

}  // namespace

const char* ToString(EnvironmentError error) noexcept {
    switch (error) {
    case EnvironmentError::None:
        return "none";
    case EnvironmentError::SystemSourceMissing:
        return "the system source path is empty";
    case EnvironmentError::EntrySourceMissing:
        return "the entry source path is empty";
    case EnvironmentError::SourceUnusable:
        return "a source path names nothing usable";
    case EnvironmentError::TreeFailed:
        return "the system source could not be read as a tree";
    case EnvironmentError::MountFailed:
        return "the system tree could not be mounted";
    case EnvironmentError::EntryFailed:
        return "the entry source could not be read as a table";
    }
    return "unknown";
}

std::optional<SystemEnvironment> LoadEnvironment(const EnvironmentConfig& config,
                                                EnvironmentError* error, std::string* detail) {
    SystemEnvironment environment;

    // --- the system source -------------------------------------------------
    if (config.system_source.empty()) {
        SetError(error, detail, EnvironmentError::SystemSourceMissing, {});
        return std::nullopt;
    }

    std::optional<ReadOnlyTree> tree;
    SourceError source_error = SourceError::None;
    if (IsHostDirectory(config.system_source, &source_error)) {
        std::string why;
        tree = ReadOnlyTree::FromHostDirectory(config.system_source, why);
        if (!tree.has_value()) {
            SetError(error, detail, EnvironmentError::TreeFailed, std::move(why));
            return std::nullopt;
        }
    } else if (source_error == SourceError::NotADirectory) {
        // Not a directory, so it has to be an image: a structural choice, made
        // by looking at the path, never by matching a filename.
        auto device = OpenHostImage(config.system_source, &source_error);
        if (!device.has_value()) {
            SetError(error, detail, EnvironmentError::SourceUnusable,
                     ToString(source_error));
            return std::nullopt;
        }
        std::string why;
        // A zip is a source form in its own right; anything else goes through
        // the container/archive readers. Both end up as one file list over the
        // same device.
        std::optional<std::vector<ReadOnlyTree::RangeFile>> files;
        if (LooksLikeZip(**device)) {
            ZipError zip_error = ZipError::None;
            const auto zip = ParseZip(**device, &zip_error);
            if (!zip.has_value()) {
                SetError(error, detail, EnvironmentError::TreeFailed,
                         std::string("the source is a zip that could not be read: ") +
                             ToString(zip_error));
                return std::nullopt;
            }
            files = ZipToRanges(*zip, why);
        } else {
            files = ExpandDeviceToFiles(*device, PartitionReaders::Default(), why);
        }
        if (!files.has_value()) {
            SetError(error, detail, EnvironmentError::TreeFailed, std::move(why));
            return std::nullopt;
        }
        tree = ReadOnlyTree::FromRanges(*device, *files, why);
        if (!tree.has_value()) {
            SetError(error, detail, EnvironmentError::TreeFailed, std::move(why));
            return std::nullopt;
        }
    } else {
        SetError(error, detail, EnvironmentError::SourceUnusable, ToString(source_error));
        return std::nullopt;
    }

    MountError mount_error = MountError::None;
    if (!environment.mounts.Mount(std::string(kSystemMountName), std::move(*tree),
                                  &mount_error)) {
        SetError(error, detail, EnvironmentError::MountFailed, ToString(mount_error));
        return std::nullopt;
    }

    // --- the entry source --------------------------------------------------
    if (config.entry_source.empty()) {
        SetError(error, detail, EnvironmentError::EntrySourceMissing, {});
        return std::nullopt;
    }

    SourceError entry_source_error = SourceError::None;
    auto bytes = ReadHostFile(config.entry_source, &entry_source_error);
    if (!bytes.has_value()) {
        SetError(error, detail, EnvironmentError::EntryFailed, ToString(entry_source_error));
        return std::nullopt;
    }
    std::string why;
    auto entries = EntryStore::FromHexTable(*bytes, why);
    if (!entries.has_value()) {
        SetError(error, detail, EnvironmentError::EntryFailed, std::move(why));
        return std::nullopt;
    }
    environment.entries = std::move(*entries);

    if (error != nullptr) {
        *error = EnvironmentError::None;
    }
    if (detail != nullptr) {
        detail->clear();
    }
    return environment;
}

}  // namespace zlong::mount
