#include "zlong/mount/tree.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

namespace zlong::mount {

namespace {

void SortRecursive(Node& node) {
    std::sort(node.children.begin(), node.children.end(),
              [](const Node& left, const Node& right) { return left.name < right.name; });
    for (Node& child : node.children) {
        if (child.kind == Node::Kind::Directory) {
            SortRecursive(child);
        }
    }
}

std::size_t CountNodes(const Node& node) {
    std::size_t total = 1;
    for (const Node& child : node.children) {
        total += CountNodes(child);
    }
    return total;
}

Node* FindDirectoryChild(Node& parent, std::string_view name) {
    for (Node& child : parent.children) {
        if (child.name == name && child.kind == Node::Kind::Directory) {
            return &child;
        }
    }
    return nullptr;
}

bool HasChild(const Node& parent, std::string_view name) {
    for (const Node& child : parent.children) {
        if (child.name == name) {
            return true;
        }
    }
    return false;
}

/// Insert one file at `path`, creating intermediate directories. Refuses a
/// duplicate, and refuses the same name being used as both a file and a
/// directory -- both would otherwise silently drop or shadow an entry.
bool InsertFile(Node& root, std::string_view path, const ssd::BlockDevice* device,
                std::uint64_t offset, std::uint64_t size, std::string& error) {
    const auto segments = SplitTreePath(path);
    if (!segments.has_value() || segments->empty()) {
        error = "the file path \"" + std::string(path) + "\" is malformed";
        return false;
    }

    Node* directory = &root;
    for (std::size_t index = 0; index + 1 < segments->size(); ++index) {
        Node* next = FindDirectoryChild(*directory, (*segments)[index]);
        if (next == nullptr) {
            if (HasChild(*directory, (*segments)[index])) {
                error = "\"" + std::string((*segments)[index]) +
                        "\" is used as both a file and a directory";
                return false;
            }
            directory->children.push_back(MakeDirectory(std::string((*segments)[index]), {}));
            next = &directory->children.back();
        }
        directory = next;
    }

    if (HasChild(*directory, segments->back())) {
        error = "the tree already has an entry named \"" + std::string(segments->back()) + "\"";
        return false;
    }
    directory->children.push_back(MakeFile(std::string(segments->back()), device, offset, size));
    return true;
}

}  // namespace

const char* ToString(TreeError error) noexcept {
    switch (error) {
    case TreeError::None:
        return "none";
    case TreeError::NotFound:
        return "not found";
    case TreeError::NotAFile:
        return "not a file";
    case TreeError::NotADirectory:
        return "not a directory";
    case TreeError::BadPath:
        return "malformed path";
    case TreeError::OutOfRange:
        return "range past the end of the file";
    case TreeError::NotAForest:
        return "the node is not a directory";
    case TreeError::NoBacking:
        return "the file has no backing device";
    case TreeError::DeviceError:
        return "the backing device refused the read";
    }
    return "unknown";
}

std::optional<std::vector<std::string_view>> SplitTreePath(std::string_view path) {
    std::vector<std::string_view> segments;
    std::string_view rest = path;
    if (!rest.empty() && rest.front() == '/') {
        rest.remove_prefix(1);
    }
    if (rest.empty()) {
        return segments;  // "" and "/" both mean the root
    }
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view segment =
            slash == std::string_view::npos ? rest : rest.substr(0, slash);
        // Anything that could leave the tree, or that the host would read
        // differently, is refused here rather than resolved.
        if (segment.empty() || segment == "." || segment == ".." ||
            segment.find('\\') != std::string_view::npos ||
            segment.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        segments.push_back(segment);
        if (slash == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(slash + 1);
    }
    return segments;
}

Node MakeDirectory(std::string name, std::vector<Node> children) {
    Node node;
    node.kind = Node::Kind::Directory;
    node.name = std::move(name);
    node.children = std::move(children);
    return node;
}

Node MakeFile(std::string name, const ssd::BlockDevice* device, std::uint64_t offset,
              std::uint64_t size) {
    Node node;
    node.kind = Node::Kind::File;
    node.name = std::move(name);
    node.device = device;
    node.offset = offset;
    node.size = size;
    return node;
}

ReadOnlyTree::ReadOnlyTree(std::vector<std::shared_ptr<const ssd::BlockDevice>> devices, Node root)
    : devices_(std::move(devices)), root_(std::move(root)) {
    root_.kind = Node::Kind::Directory;
    root_.name.clear();
    root_.device = nullptr;
    root_.offset = 0;
    root_.size = 0;
    SortRecursive(root_);
}

std::optional<ReadOnlyTree> ReadOnlyTree::FromMemoryFiles(std::vector<MemoryFile> files,
                                                          std::string& error) {
    std::vector<std::shared_ptr<const ssd::BlockDevice>> devices;
    devices.reserve(files.size());
    Node root = MakeDirectory("", {});

    for (MemoryFile& file : files) {
        auto device = std::make_shared<ssd::MemoryBlockDevice>(std::move(file.bytes));
        const std::uint64_t size = device->size();
        if (!InsertFile(root, file.path, device.get(), 0, size, error)) {
            return std::nullopt;
        }
        devices.push_back(std::move(device));
    }

    error.clear();
    return ReadOnlyTree(std::move(devices), std::move(root));
}

std::optional<ReadOnlyTree> ReadOnlyTree::FromRanges(std::shared_ptr<const ssd::BlockDevice> device,
                                                     std::vector<RangeFile> files,
                                                     std::string& error) {
    if (device == nullptr) {
        error = "the file list has no backing device";
        return std::nullopt;
    }
    Node root = MakeDirectory("", {});
    for (const RangeFile& file : files) {
        if (!InsertFile(root, file.path, device.get(), file.offset, file.size, error)) {
            return std::nullopt;
        }
    }
    error.clear();
    std::vector<std::shared_ptr<const ssd::BlockDevice>> devices;
    devices.push_back(std::move(device));
    return ReadOnlyTree(std::move(devices), std::move(root));
}

std::optional<ReadOnlyTree> ReadOnlyTree::FromHostDirectory(const std::string& path,
                                                            std::string& error) {
    namespace fs = std::filesystem;
    if (path.empty()) {
        error = "the directory path is empty";
        return std::nullopt;
    }
    // Build the path from char8_t so a non-ASCII path converts through UTF-16
    // rather than the ANSI code page.
    const std::u8string u8(reinterpret_cast<const char8_t*>(path.data()), path.size());
    const fs::path root(u8);

    std::error_code code;
    if (!fs::is_directory(root, code)) {
        error = "\"" + path + "\" is not a directory";
        return std::nullopt;
    }

    std::vector<MemoryFile> files;
    // No follow_directory_symlink, so a self-referential link cannot spin here.
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                             code),
         end;
         !code && it != end; it.increment(code)) {
        const fs::directory_entry& entry = *it;
        std::error_code entry_code;
        if (!entry.is_regular_file(entry_code) || entry_code) {
            continue;
        }
        const fs::path relative = fs::relative(entry.path(), root, entry_code);
        if (entry_code) {
            continue;
        }
        const std::u8string name_u8 = relative.generic_u8string();

        MemoryFile file;
        file.path.assign(reinterpret_cast<const char*>(name_u8.data()), name_u8.size());

        std::ifstream stream(entry.path(), std::ios::in | std::ios::binary);
        if (!stream.is_open()) {
            error = "could not open \"" + file.path + "\"";
            return std::nullopt;
        }
        stream.seekg(0, std::ios::end);
        const std::streamoff size = stream.tellg();
        if (size < 0) {
            error = "could not size \"" + file.path + "\"";
            return std::nullopt;
        }
        stream.seekg(0, std::ios::beg);
        file.bytes.resize(static_cast<std::size_t>(size));
        if (!file.bytes.empty()) {
            stream.read(reinterpret_cast<char*>(file.bytes.data()),
                        static_cast<std::streamsize>(file.bytes.size()));
            if (stream.gcount() != static_cast<std::streamsize>(file.bytes.size())) {
                error = "could not read \"" + file.path + "\"";
                return std::nullopt;
            }
        }
        files.push_back(std::move(file));
    }
    if (code) {
        error = "listing \"" + path + "\" failed: " + code.message();
        return std::nullopt;
    }
    return FromMemoryFiles(std::move(files), error);
}

const Node* ReadOnlyTree::Find(std::string_view path, TreeError* error) const {
    const auto fail = [error](TreeError reason) -> const Node* {
        if (error != nullptr) {
            *error = reason;
        }
        return nullptr;
    };

    const auto segments = SplitTreePath(path);
    if (!segments.has_value()) {
        return fail(TreeError::BadPath);
    }

    const Node* node = &root_;
    for (const std::string_view segment : *segments) {
        if (node->kind != Node::Kind::Directory) {
            return fail(TreeError::NotADirectory);
        }
        const auto found =
            std::lower_bound(node->children.begin(), node->children.end(), segment,
                             [](const Node& child, std::string_view name) {
                                 return child.name < name;
                             });
        if (found == node->children.end() || found->name != segment) {
            return fail(TreeError::NotFound);
        }
        node = &*found;
    }
    if (error != nullptr) {
        *error = TreeError::None;
    }
    return node;
}

bool ReadOnlyTree::Exists(std::string_view path) const { return Find(path) != nullptr; }

std::vector<std::string> ReadOnlyTree::List(std::string_view path, TreeError* error) const {
    std::vector<std::string> names;
    const Node* node = Find(path, error);
    if (node == nullptr) {
        return names;
    }
    if (node->kind != Node::Kind::Directory) {
        if (error != nullptr) {
            *error = TreeError::NotADirectory;
        }
        return names;
    }
    names.reserve(node->children.size());
    for (const Node& child : node->children) {
        names.push_back(child.name);
    }
    return names;
}

std::optional<std::uint64_t> ReadOnlyTree::Size(std::string_view path, TreeError* error) const {
    const Node* node = Find(path, error);
    if (node == nullptr) {
        return std::nullopt;
    }
    if (node->kind != Node::Kind::File) {
        if (error != nullptr) {
            *error = TreeError::NotAFile;
        }
        return std::nullopt;
    }
    return node->size;
}

std::optional<std::vector<std::uint8_t>> ReadOnlyTree::ReadAt(std::string_view path,
                                                              std::uint64_t offset,
                                                              std::size_t length,
                                                              TreeError* error) const {
    const Node* node = Find(path, error);
    if (node == nullptr) {
        return std::nullopt;
    }
    if (node->kind != Node::Kind::File) {
        if (error != nullptr) {
            *error = TreeError::NotAFile;
        }
        return std::nullopt;
    }
    if (node->device == nullptr) {
        if (error != nullptr) {
            *error = TreeError::NoBacking;
        }
        return std::nullopt;
    }
    // Refuse rather than clamp: a short read would silently look like
    // truncated data to everything above.
    if (offset > node->size || length > node->size - offset) {
        if (error != nullptr) {
            *error = TreeError::OutOfRange;
        }
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(length);
    if (length != 0 && !node->device->ReadAt(node->offset + offset, bytes.data(), length)) {
        if (error != nullptr) {
            *error = TreeError::DeviceError;
        }
        return std::nullopt;
    }
    if (error != nullptr) {
        *error = TreeError::None;
    }
    return bytes;
}

std::optional<std::vector<std::uint8_t>> ReadOnlyTree::Read(std::string_view path,
                                                            TreeError* error) const {
    const Node* node = Find(path, error);
    if (node == nullptr) {
        return std::nullopt;
    }
    if (node->kind != Node::Kind::File) {
        if (error != nullptr) {
            *error = TreeError::NotAFile;
        }
        return std::nullopt;
    }
    if (node->size > std::numeric_limits<std::size_t>::max()) {
        if (error != nullptr) {
            *error = TreeError::OutOfRange;
        }
        return std::nullopt;
    }
    return ReadAt(path, 0, static_cast<std::size_t>(node->size), error);
}

std::size_t ReadOnlyTree::node_count() const noexcept { return CountNodes(root_); }

}  // namespace zlong::mount
