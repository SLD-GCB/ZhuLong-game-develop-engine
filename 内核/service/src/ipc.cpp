#include "zlong/service/ipc.h"

namespace zlong::service {

namespace {

void SetError(IpcError* error, IpcError value) {
    if (error != nullptr) {
        *error = value;
    }
}

}  // namespace

const char* ToString(IpcError error) noexcept {
    switch (error) {
    case IpcError::None:
        return "none";
    case IpcError::HeaderUnreadable:
        return "the command buffer header could not be read";
    case IpcError::BadMagic:
        return "the command buffer magic does not match";
    case IpcError::TooLarge:
        return "a length the guest gave is out of range";
    case IpcError::TooManyBuffers:
        return "the guest described too many buffers";
    case IpcError::DataUnreadable:
        return "the inline data could not be read";
    case IpcError::BufferTableUnreadable:
        return "the buffer table could not be read";
    case IpcError::BufferUnreachable:
        return "a buffer the guest named is not reachable";
    case IpcError::ResponseTooLarge:
        return "the response does not fit where the guest asked for it";
    }
    return "unknown";
}

bool UnpackRequest(cpu::GuestMemory& memory, std::uint64_t address, IpcRequest& request,
                   IpcError* error) {
    // Nothing is returned half-unpacked: start empty and only hand it back whole.
    request = IpcRequest{};

    const auto magic = memory.TryRead32(address + 0);
    const auto command = memory.TryRead32(address + 4);
    const auto data_size = memory.TryRead32(address + 8);
    const auto buffer_count = memory.TryRead32(address + 12);
    if (!magic.has_value() || !command.has_value() || !data_size.has_value() ||
        !buffer_count.has_value()) {
        SetError(error, IpcError::HeaderUnreadable);
        return false;
    }
    if (*magic != kIpcMagic) {
        SetError(error, IpcError::BadMagic);
        return false;
    }
    if (*data_size > kMaxIpcDataBytes || *buffer_count > kMaxIpcBuffers) {
        SetError(error, *data_size > kMaxIpcDataBytes ? IpcError::TooLarge
                                                     : IpcError::TooManyBuffers);
        return false;
    }

    request.command_id = *command;
    request.data.resize(*data_size);
    for (std::uint32_t index = 0; index < *data_size; ++index) {
        const auto byte = memory.TryRead8(address + kIpcHeaderBytes + index);
        if (!byte.has_value()) {
            request = IpcRequest{};
            SetError(error, IpcError::DataUnreadable);
            return false;
        }
        request.data[index] = *byte;
    }

    const std::uint64_t table = address + kIpcHeaderBytes + *data_size;
    request.buffers.reserve(*buffer_count);
    for (std::uint32_t index = 0; index < *buffer_count; ++index) {
        const std::uint64_t entry = table + static_cast<std::uint64_t>(index) * kIpcBufferEntryBytes;
        const auto buffer_address = memory.TryRead64(entry + 0);
        const auto buffer_size = memory.TryRead64(entry + 8);
        const auto mode = memory.TryRead32(entry + 16);
        if (!buffer_address.has_value() || !buffer_size.has_value() || !mode.has_value()) {
            request = IpcRequest{};
            SetError(error, IpcError::BufferTableUnreadable);
            return false;
        }

        IpcBuffer buffer;
        buffer.address = *buffer_address;
        buffer.size = *buffer_size;
        buffer.mode = *mode;

        // A buffer the guest named has to be reachable NOW, so a service never
        // gets a descriptor it cannot use. Both ends are probed: a full read
        // would copy what the service may not even want, and a hole in the
        // middle is the service's to notice when it reads.
        if (buffer.size != 0) {
            const auto first = memory.TryRead8(buffer.address);
            const auto last = memory.TryRead8(buffer.address + buffer.size - 1);
            if (!first.has_value() || !last.has_value()) {
                request = IpcRequest{};
                SetError(error, IpcError::BufferUnreachable);
                return false;
            }
        }
        request.buffers.push_back(buffer);
    }

    SetError(error, IpcError::None);
    return true;
}

bool PackResponse(cpu::GuestMemory& memory, std::uint64_t address, std::uint64_t room,
                  const IpcResponse& response, IpcError* error) {
    if (response.data.size() + kIpcHeaderBytes > room) {
        SetError(error, IpcError::ResponseTooLarge);
        return false;
    }
    if (!memory.TryWrite32(address + 0, kIpcMagic) ||
        !memory.TryWrite32(address + 4, response.result) ||
        !memory.TryWrite32(address + 8, static_cast<std::uint32_t>(response.data.size())) ||
        !memory.TryWrite32(address + 12, 0)) {
        SetError(error, IpcError::ResponseTooLarge);
        return false;
    }
    for (std::size_t index = 0; index < response.data.size(); ++index) {
        if (!memory.TryWrite8(address + kIpcHeaderBytes + index, response.data[index])) {
            SetError(error, IpcError::ResponseTooLarge);
            return false;
        }
    }
    SetError(error, IpcError::None);
    return true;
}

}  // namespace zlong::service
