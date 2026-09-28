// 烛龙 (ZhuLong) - fsp-srv: the guest's filesystem.
//
// This is where the mount layer becomes something a guest can use. A path is
// addressed as "<mount>/<path inside that tree>" -- the same shape
// mount::MountTable::Resolve takes, and the first segment is what picks the
// tree. So "/system/abc" reads abc out of whatever is mounted as "system".
//
// ============================================================================
// PLACEHOLDER, like every other command layout in this layer: the command ids
// below, and the way a path and a byte range travel in the request's data. The
// console's protocol -- its own request structs, its handle semantics, its error
// codes -- is not transcribed.
// ============================================================================

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "zlong/service/registry.h"

namespace zlong::service {

/// Failure results, in the same placeholder family as the other services'.
enum : std::uint32_t {
    kFspResultBadArgument = 0x5A46'0001u,
    kFspResultNotFound = 0x5A46'0002u,
    kFspResultBadHandle = 0x5A46'0003u,
    kFspResultReadFailed = 0x5A46'0004u,
    kFspResultNotAFile = 0x5A46'0005u,
};

class FspService final : public IService {
public:
    enum Command : std::uint32_t {
        /// data: none. response: u32 count, then per mount u32 length + bytes.
        kCommandGetMounts = 1,
        /// data: the path. response: u32 file id, u64 size.
        kCommandOpenFile = 2,
        /// data: u32 file id, u32 offset, u32 length. response: the bytes.
        kCommandReadFile = 3,
        /// data: u32 file id. response: none.
        kCommandCloseFile = 4,
        /// data: the path. response: u32 kind, 0 for a file and 1 for a directory.
        kCommandGetEntryType = 5,
    };

    /// What kind of node a path names, as reported in a response.
    enum EntryKind : std::uint32_t { kEntryFile = 0, kEntryDirectory = 1 };

    const char* name() const noexcept override { return "fsp-srv"; }

    void HandleRequest(ServiceContext& context, const IpcRequest& request,
                       IpcResponse& response) override;

    // --- what it has done, for tests and diagnosis --------------------------
    std::uint32_t opens() const noexcept { return opens_; }
    std::uint32_t reads() const noexcept { return reads_; }
    std::uint32_t closes() const noexcept { return closes_; }
    std::uint32_t unknown_commands() const noexcept { return unknown_commands_; }
    /// Open file ids that were never closed. A leak, and visible as one.
    std::size_t open_files() const noexcept { return open_files_.size(); }

    /// The reason the last refusal carries. Empty after a success.
    const std::string& last_error() const noexcept { return last_error_; }

private:
    /// An open file. The path is kept rather than a tree pointer: a file is a
    /// range of a tree that a later unmount could replace, and re-resolving on
    /// every read is what makes that show up as a refusal instead of as a read
    /// from a tree that is no longer mounted.
    struct File {
        /// Who opened it. A file id is a number, and one guest must not be able
        /// to read another's by guessing it.
        std::uint64_t session = 0;
        std::string path;
        std::uint64_t size = 0;
    };

    void GetMounts(ServiceContext& context, IpcResponse& response);
    void OpenFile(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void ReadFile(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void CloseFile(ServiceContext& context, const IpcRequest& request, IpcResponse& response);
    void GetEntryType(ServiceContext& context, const IpcRequest& request, IpcResponse& response);

    bool Refuse(IpcResponse& response, std::uint32_t result, const std::string& why);

    std::map<std::uint32_t, File> open_files_;
    std::uint32_t next_file_id_ = 1;

    std::uint32_t opens_ = 0;
    std::uint32_t reads_ = 0;
    std::uint32_t closes_ = 0;
    std::uint32_t unknown_commands_ = 0;
    std::string last_error_;
};

}  // namespace zlong::service
