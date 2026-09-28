#include "zlong/mount/mount.h"

#include <utility>

namespace zlong::mount {

namespace {

void SetError(MountError* error, MountError value) {
    if (error != nullptr) {
        *error = value;
    }
}

}  // namespace

const char* ToString(MountError error) noexcept {
    switch (error) {
    case MountError::None:
        return "none";
    case MountError::BadName:
        return "malformed mount name";
    case MountError::AlreadyMounted:
        return "a source is already mounted there";
    case MountError::NotFound:
        return "no source is mounted at that name";
    case MountError::BadPath:
        return "malformed path";
    case MountError::Tree:
        return "the mounted tree refused the operation";
    }
    return "unknown";
}

bool MountTable::Mount(std::string name, ReadOnlyTree tree, MountError* error) {
    const bool malformed = name.empty() || name == "." || name == ".." ||
                           name.find('/') != std::string::npos ||
                           name.find('\\') != std::string::npos ||
                           name.find('\0') != std::string::npos;
    if (malformed) {
        SetError(error, MountError::BadName);
        return false;
    }
    if (trees_.find(name) != trees_.end()) {
        SetError(error, MountError::AlreadyMounted);
        return false;
    }
    trees_.emplace(std::move(name), std::move(tree));
    SetError(error, MountError::None);
    return true;
}

bool MountTable::Unmount(std::string_view name) {
    // map::erase needs a key_type; the transparent comparator only covers find.
    const auto it = trees_.find(name);
    if (it == trees_.end()) {
        return false;
    }
    trees_.erase(it);
    return true;
}

bool MountTable::mounted(std::string_view name) const {
    return trees_.find(name) != trees_.end();
}

std::vector<std::string> MountTable::mounts() const {
    std::vector<std::string> names;
    names.reserve(trees_.size());
    for (const auto& entry : trees_) {
        names.push_back(entry.first);
    }
    return names;
}

const ReadOnlyTree* MountTable::Find(std::string_view name) const {
    const auto it = trees_.find(name);
    return it == trees_.end() ? nullptr : &it->second;
}

std::optional<MountTable::Resolved> MountTable::Resolve(std::string_view path,
                                                        MountError* error) const {
    const auto segments = SplitTreePath(path);
    if (!segments.has_value() || segments->empty() || (*segments)[0].empty()) {
        SetError(error, MountError::BadPath);
        return std::nullopt;
    }

    const auto it = trees_.find((*segments)[0]);
    if (it == trees_.end()) {
        SetError(error, MountError::NotFound);
        return std::nullopt;
    }

    std::string relative;
    for (std::size_t index = 1; index < segments->size(); ++index) {
        if (!relative.empty()) {
            relative.push_back('/');
        }
        relative.append((*segments)[index]);
    }

    SetError(error, MountError::None);
    return Resolved{&it->second, std::move(relative)};
}

std::optional<std::vector<std::uint8_t>> MountTable::Read(std::string_view path,
                                                          MountError* error) const {
    const auto resolved = Resolve(path, error);
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto bytes = resolved->tree->Read(resolved->path);
    if (!bytes.has_value()) {
        SetError(error, MountError::Tree);
        return std::nullopt;
    }
    SetError(error, MountError::None);
    return bytes;
}

bool MountTable::Exists(std::string_view path) const {
    const auto resolved = Resolve(path);
    return resolved.has_value() && resolved->tree->Exists(resolved->path);
}

std::size_t MountTable::node_count() const noexcept {
    std::size_t total = 0;
    for (const auto& entry : trees_) {
        total += entry.second.node_count();
    }
    return total;
}

}  // namespace zlong::mount
