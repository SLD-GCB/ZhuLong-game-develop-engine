#include "zlong/service/services/fsp_service.h"

#include <cstddef>
#include <vector>

#include "../word_io.h"
#include "zlong/service/ipc.h"
#include "zlong/service/kernel.h"
#include "zlong/service/process.h"

namespace zlong::service {

using word_io::PutBytes;
using word_io::PutU32;
using word_io::PutU64;
using word_io::Text;
using word_io::Words;

namespace {

/// A read the guest may ask for in one go. The response is bounded by what the
/// guest offered room for anyway, so this only stops a huge length from making
/// the emulator allocate before anyone has checked anything.
constexpr std::uint32_t kMaxReadBytes = kMaxIpcDataBytes;

/// The mounts the caller resolves paths through, or nullptr. Null means the
/// process has none -- which is a refusal with a reason, not an empty tree.
const mount::MountTable* MountsOf(ServiceContext& context) {
    KProcess* process = context.kernel.process();
    return process == nullptr ? nullptr : process->mounts();
}

}  // namespace

void FspService::HandleRequest(ServiceContext& context, const IpcRequest& request,
                               IpcResponse& response) {
    switch (request.command_id) {
    case kCommandGetMounts:
        GetMounts(context, response);
        return;
    case kCommandOpenFile:
        OpenFile(context, request, response);
        return;
    case kCommandReadFile:
        ReadFile(context, request, response);
        return;
    case kCommandCloseFile:
        CloseFile(context, request, response);
        return;
    case kCommandGetEntryType:
        GetEntryType(context, request, response);
        return;
    default:
        ++unknown_commands_;
        response.result = kIpcResultUnknownCommand;
        return;
    }
}

void FspService::GetMounts(ServiceContext& context, IpcResponse& response) {
    const mount::MountTable* mounts = MountsOf(context);
    if (mounts == nullptr) {
        Refuse(response, kFspResultNotFound, "the process has no mounted sources");
        return;
    }

    const std::vector<std::string> names = mounts->mounts();
    PutU32(response.data, static_cast<std::uint32_t>(names.size()));
    for (const std::string& name : names) {
        PutU32(response.data, static_cast<std::uint32_t>(name.size()));
        PutBytes(response.data, name.data(), name.size());
    }

    last_error_.clear();
    response.result = 0;
}

void FspService::OpenFile(ServiceContext& context, const IpcRequest& request,
                          IpcResponse& response) {
    const mount::MountTable* mounts = MountsOf(context);
    if (mounts == nullptr) {
        Refuse(response, kFspResultNotFound, "the process has no mounted sources");
        return;
    }

    const std::string path = Text(request.data);
    if (path.empty()) {
        Refuse(response, kFspResultBadArgument, "the path is empty");
        return;
    }

    mount::MountError mount_error = mount::MountError::None;
    const auto resolved = mounts->Resolve(path, &mount_error);
    if (!resolved) {
        Refuse(response, kFspResultNotFound,
               std::string("could not resolve the path: ") + mount::ToString(mount_error));
        return;
    }

    mount::TreeError tree_error = mount::TreeError::None;
    const mount::Node* node = resolved->tree->Find(resolved->path, &tree_error);
    if (node == nullptr) {
        Refuse(response, kFspResultNotFound,
               std::string("no such entry: ") + mount::ToString(tree_error));
        return;
    }
    if (node->kind != mount::Node::Kind::File) {
        Refuse(response, kFspResultNotAFile, "that path names a directory");
        return;
    }

    const std::uint32_t id = next_file_id_++;
    auto file = File{};
    file.session = context.session.session_id();
    file.path = path;
    file.size = node->size;
    open_files_[id] = std::move(file);
    ++opens_;

    last_error_.clear();
    response.result = 0;
    PutU32(response.data, id);
    PutU64(response.data, node->size);
}

void FspService::ReadFile(ServiceContext& context, const IpcRequest& request,
                          IpcResponse& response) {
    Words words(request.data);
    std::uint32_t id = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    if (!words.U32(id) || !words.U32(offset) || !words.U32(length)) {
        Refuse(response, kFspResultBadArgument, "a read needs an id, an offset and a length");
        return;
    }
    if (length > kMaxReadBytes) {
        Refuse(response, kFspResultBadArgument, "the read is larger than a response can carry");
        return;
    }

    const auto found = open_files_.find(id);
    if (found == open_files_.end() || found->second.session != context.session.session_id()) {
        Refuse(response, kFspResultBadHandle, "that file id is not open here");
        return;
    }

    const mount::MountTable* mounts = MountsOf(context);
    if (mounts == nullptr) {
        Refuse(response, kFspResultNotFound, "the process has no mounted sources");
        return;
    }

    // Re-resolved rather than remembered: if the tree was unmounted since the
    // open, that has to be a refusal, not a read out of a tree that is gone.
    mount::MountError mount_error = mount::MountError::None;
    const auto resolved = mounts->Resolve(found->second.path, &mount_error);
    if (!resolved) {
        Refuse(response, kFspResultNotFound,
               std::string("the file's mount is gone: ") + mount::ToString(mount_error));
        return;
    }

    mount::TreeError tree_error = mount::TreeError::None;
    const auto bytes = resolved->tree->ReadAt(resolved->path, offset, length, &tree_error);
    if (!bytes) {
        // A range past the end is refused by the tree, never clamped: a short
        // read would look like truncated data and nothing would say so.
        Refuse(response, kFspResultReadFailed,
               std::string("the read failed: ") + mount::ToString(tree_error));
        return;
    }

    response.data = *bytes;
    ++reads_;
    last_error_.clear();
    response.result = 0;
}

void FspService::CloseFile(ServiceContext& context, const IpcRequest& request,
                           IpcResponse& response) {
    Words words(request.data);
    std::uint32_t id = 0;
    if (!words.U32(id)) {
        Refuse(response, kFspResultBadArgument, "a close needs a file id");
        return;
    }

    const auto found = open_files_.find(id);
    if (found == open_files_.end() || found->second.session != context.session.session_id()) {
        Refuse(response, kFspResultBadHandle, "that file id is not open here");
        return;
    }

    open_files_.erase(found);
    ++closes_;
    last_error_.clear();
    response.result = 0;
}

void FspService::GetEntryType(ServiceContext& context, const IpcRequest& request,
                              IpcResponse& response) {
    const mount::MountTable* mounts = MountsOf(context);
    if (mounts == nullptr) {
        Refuse(response, kFspResultNotFound, "the process has no mounted sources");
        return;
    }

    const std::string path = Text(request.data);
    if (path.empty()) {
        Refuse(response, kFspResultBadArgument, "the path is empty");
        return;
    }

    mount::MountError mount_error = mount::MountError::None;
    const auto resolved = mounts->Resolve(path, &mount_error);
    if (!resolved) {
        Refuse(response, kFspResultNotFound,
               std::string("could not resolve the path: ") + mount::ToString(mount_error));
        return;
    }

    mount::TreeError tree_error = mount::TreeError::None;
    const mount::Node* node = resolved->tree->Find(resolved->path, &tree_error);
    if (node == nullptr) {
        Refuse(response, kFspResultNotFound,
               std::string("no such entry: ") + mount::ToString(tree_error));
        return;
    }

    last_error_.clear();
    response.result = 0;
    PutU32(response.data,
           node->kind == mount::Node::Kind::Directory ? kEntryDirectory : kEntryFile);
}

bool FspService::Refuse(IpcResponse& response, std::uint32_t result, const std::string& why) {
    response.result = result;
    response.data.clear();
    last_error_ = why;
    return false;
}

}  // namespace zlong::service
