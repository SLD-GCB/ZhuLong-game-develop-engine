#include "zlong/mount/source.h"

#include <filesystem>
#include <fstream>

namespace zlong::mount {

namespace {

namespace fs = std::filesystem;

/// A UTF-8 string as a std::filesystem::path.
///
/// The narrow `fstream`/`path` constructors go through the ANSI code page on
/// Windows, so a path with non-ASCII characters fails there. Building the path
/// from char8_t makes the conversion go through UTF-16 instead, which is the
/// encoding Windows actually stores paths in.
fs::path PathFromUtf8(const std::string& utf8) {
    const std::u8string wide(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return fs::path(wide);
}

void SetError(SourceError* error, SourceError value) {
    if (error != nullptr) {
        *error = value;
    }
}

}  // namespace

const char* ToString(SourceError error) noexcept {
    switch (error) {
    case SourceError::None:
        return "none";
    case SourceError::EmptyPath:
        return "the path is empty";
    case SourceError::NotFound:
        return "the path does not exist";
    case SourceError::NotAFile:
        return "the path is not a regular file";
    case SourceError::NotADirectory:
        return "the path is not a directory";
    case SourceError::HostError:
        return "the host refused to open the path";
    case SourceError::DeviceError:
        return "the opened file could not be read";
    }
    return "unknown";
}

struct HostFileDevice::Impl {
    std::ifstream stream;
};

HostFileDevice::HostFileDevice(HostFileDevice&& other) noexcept
    : impl_(other.impl_), size_(other.size_) {
    other.impl_ = nullptr;
    other.size_ = 0;
}

HostFileDevice& HostFileDevice::operator=(HostFileDevice&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        size_ = other.size_;
        other.impl_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

HostFileDevice::~HostFileDevice() { delete impl_; }

std::optional<HostFileDevice> HostFileDevice::Open(const std::string& utf8_path) {
    if (utf8_path.empty()) {
        return std::nullopt;
    }
    const fs::path path = PathFromUtf8(utf8_path);

    std::error_code code;
    if (!fs::is_regular_file(path, code)) {
        return std::nullopt;
    }

    auto impl = std::make_unique<Impl>();
    impl->stream.open(path, std::ios::in | std::ios::binary);
    if (!impl->stream.is_open()) {
        return std::nullopt;
    }
    impl->stream.seekg(0, std::ios::end);
    const std::streamoff end = impl->stream.tellg();
    if (end < 0) {
        return std::nullopt;
    }
    impl->stream.seekg(0, std::ios::beg);

    HostFileDevice device;
    device.impl_ = impl.release();
    device.size_ = static_cast<std::uint64_t>(end);
    return device;
}

bool HostFileDevice::ReadAt(std::uint64_t offset, void* dst, std::size_t n) const {
    if (impl_ == nullptr || dst == nullptr) {
        return false;
    }
    if (offset > size_ || n > size_ - offset) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    impl_->stream.clear();
    impl_->stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    impl_->stream.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    // gcount, not good(): a read that lands exactly on the end still counts as
    // success, and eofbit is not a failure here.
    return impl_->stream.gcount() == static_cast<std::streamsize>(n);
}

bool HostFileDevice::WriteAt(std::uint64_t, const void*, std::size_t) { return false; }

std::optional<std::shared_ptr<const ssd::BlockDevice>> OpenHostImage(const std::string& utf8_path,
                                                                    SourceError* error) {
    if (utf8_path.empty()) {
        SetError(error, SourceError::EmptyPath);
        return std::nullopt;
    }
    const fs::path path = PathFromUtf8(utf8_path);

    std::error_code code;
    if (!fs::exists(path, code)) {
        SetError(error, SourceError::NotFound);
        return std::nullopt;
    }
    if (!fs::is_regular_file(path, code)) {
        SetError(error, SourceError::NotAFile);
        return std::nullopt;
    }

    auto device = HostFileDevice::Open(utf8_path);
    if (!device.has_value()) {
        SetError(error, SourceError::HostError);
        return std::nullopt;
    }
    SetError(error, SourceError::None);
    return std::shared_ptr<const ssd::BlockDevice>(
        std::make_shared<HostFileDevice>(std::move(*device)));
}

std::optional<std::vector<std::uint8_t>> ReadHostFile(const std::string& utf8_path,
                                                      SourceError* error) {
    if (utf8_path.empty()) {
        SetError(error, SourceError::EmptyPath);
        return std::nullopt;
    }
    const fs::path path = PathFromUtf8(utf8_path);

    std::error_code code;
    if (!fs::exists(path, code)) {
        SetError(error, SourceError::NotFound);
        return std::nullopt;
    }
    if (!fs::is_regular_file(path, code)) {
        SetError(error, SourceError::NotAFile);
        return std::nullopt;
    }

    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        SetError(error, SourceError::HostError);
        return std::nullopt;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff end = stream.tellg();
    if (end < 0) {
        SetError(error, SourceError::DeviceError);
        return std::nullopt;
    }
    stream.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
            SetError(error, SourceError::DeviceError);
            return std::nullopt;
        }
    }
    SetError(error, SourceError::None);
    return bytes;
}

bool IsHostDirectory(const std::string& utf8_path, SourceError* error) {
    if (utf8_path.empty()) {
        SetError(error, SourceError::EmptyPath);
        return false;
    }
    const fs::path path = PathFromUtf8(utf8_path);
    std::error_code code;
    if (!fs::exists(path, code)) {
        SetError(error, SourceError::NotFound);
        return false;
    }
    const bool directory = fs::is_directory(path, code);
    if (!directory) {
        SetError(error, SourceError::NotADirectory);
        return false;
    }
    SetError(error, SourceError::None);
    return true;
}

}  // namespace zlong::mount
