#include "zlong/ssd/block_device.h"

#include <cstring>
#include <fstream>
#include <memory>
#include <utility>

namespace zlong::ssd {

// ---------------------------------------------------------- BlockDevice ----

std::uint64_t BlockDevice::sector_count() const noexcept {
    const std::uint32_t stride = sector_size();
    return stride == 0 ? 0 : size() / stride;
}

bool BlockDevice::ReadSectors(std::uint64_t lba, void* dst, std::uint32_t count) const {
    const std::uint64_t stride = sector_size();
    if (stride == 0) {
        return false;
    }
    const std::uint64_t offset = lba * stride;
    const std::uint64_t bytes = static_cast<std::uint64_t>(count) * stride;
    if (offset > size() || bytes > size() - offset) {
        return false;
    }
    return ReadAt(offset, dst, static_cast<std::size_t>(bytes));
}

bool BlockDevice::WriteSectors(std::uint64_t lba, const void* src, std::uint32_t count) {
    const std::uint64_t stride = sector_size();
    if (stride == 0) {
        return false;
    }
    const std::uint64_t offset = lba * stride;
    const std::uint64_t bytes = static_cast<std::uint64_t>(count) * stride;
    if (offset > size() || bytes > size() - offset) {
        return false;
    }
    return WriteAt(offset, src, static_cast<std::size_t>(bytes));
}

// --------------------------------------------------- MemoryBlockDevice -----

bool MemoryBlockDevice::ReadAt(std::uint64_t offset, void* dst, std::size_t n) const {
    if (offset > data_.size() || n > data_.size() - static_cast<std::size_t>(offset)) {
        return false;
    }
    if (n != 0) {
        std::memcpy(dst, data_.data() + offset, n);
    }
    return true;
}

bool MemoryBlockDevice::WriteAt(std::uint64_t offset, const void* src, std::size_t n) {
    if (offset > data_.size() || n > data_.size() - static_cast<std::size_t>(offset)) {
        return false;
    }
    if (n != 0) {
        std::memcpy(data_.data() + offset, src, n);
    }
    return true;
}

// ----------------------------------------------------- FileBlockDevice -----

struct FileBlockDevice::Impl {
    std::fstream stream;
};

FileBlockDevice::~FileBlockDevice() {
    delete impl_;
}

bool FileBlockDevice::Open(const char* path, FileBlockDevice& out, bool create,
                           std::uint64_t create_size) {
    const std::ios::openmode mode = std::ios::in | std::ios::out | std::ios::binary;

    auto impl = std::make_unique<Impl>();
    impl->stream.open(path, mode);

    if (!impl->stream.is_open() && create) {
        // Create it, then reopen read-write. An existing file is left alone.
        impl->stream.clear();
        {
            std::ofstream creator(path, std::ios::binary | std::ios::trunc);
            if (!creator.is_open()) {
                return false;
            }
            if (create_size != 0) {
                creator.seekp(static_cast<std::streamoff>(create_size - 1));
                creator.put('\0');
                if (!creator.good()) {
                    return false;
                }
            }
        }
        impl->stream.clear();
        impl->stream.open(path, mode);
    }

    if (!impl->stream.is_open()) {
        return false;
    }

    impl->stream.seekg(0, std::ios::end);
    const std::streamoff end = impl->stream.tellg();
    if (end < 0) {
        return false;
    }

    delete out.impl_;
    out.impl_ = impl.release();
    out.size_ = static_cast<std::uint64_t>(end);
    return true;
}

bool FileBlockDevice::ReadAt(std::uint64_t offset, void* dst, std::size_t n) const {
    if (impl_ == nullptr || offset > size_ || n > size_ - offset) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    impl_->stream.clear();
    impl_->stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    impl_->stream.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return impl_->stream.gcount() == static_cast<std::streamsize>(n);
}

bool FileBlockDevice::WriteAt(std::uint64_t offset, const void* src, std::size_t n) {
    if (impl_ == nullptr || offset > size_ || n > size_ - offset) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    impl_->stream.clear();
    impl_->stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    impl_->stream.write(static_cast<const char*>(src), static_cast<std::streamsize>(n));
    return impl_->stream.good();
}

}  // namespace zlong::ssd
