// 烛龙 (ZhuLong) - IPC: the command buffer, and the boundary it sits on.
//
// This is where guest-supplied pointers and lengths arrive, so it is the one
// place in the kernel that must treat everything as hostile. Every field is read
// through GuestMemory, which reports failure instead of throwing, and NOTHING is
// returned half-unpacked: a request that could not be read in full comes back
// empty with a reason. A partially filled request would let a service act on
// garbage while looking like it had been given data.
//
// ============================================================================
// LAYOUT: this command buffer is THIS PROJECT'S PLACEHOLDER, like the syscall
// numbers. The console's real layout has to be transcribed. It lives here so
// replacing it is one edit.
//
//   offset 0    u32  magic         'ZLIP'
//   offset 4    u32  command_id
//   offset 8    u32  data_size
//   offset 12   u32  buffer_count
//   offset 16   data[data_size]
//   then        buffer_count x { u64 address; u64 size; u32 mode; u32 reserved }
//
// A response is the same header with `command_id` replaced by `result` and no
// buffer table:
//
//   offset 0    u32  magic
//   offset 4    u32  result
//   offset 8    u32  data_size
//   offset 12   u32  reserved
//   offset 16   data[data_size]
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "zlong/cpu/memory.h"

namespace zlong::service {

inline constexpr std::uint32_t kIpcMagic = 0x5A4C'4950u;  // 'ZLIP'
inline constexpr std::size_t kIpcHeaderBytes = 16;
inline constexpr std::size_t kIpcBufferEntryBytes = 24;
/// Bounds chosen so a corrupt header cannot ask for a huge allocation before any
/// range check has run.
inline constexpr std::uint32_t kMaxIpcDataBytes = 1u << 20;
inline constexpr std::uint32_t kMaxIpcBuffers = 64;
inline constexpr std::uint32_t kIpcBufferModeNormal = 0;

/// A buffer the guest described. Held as an address, not a pointer: the handler
/// may park, and a guest address stays meaningful where a host pointer would not.
struct IpcBuffer {
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    std::uint32_t mode = 0;
};

struct IpcRequest {
    std::uint32_t command_id = 0;
    /// Copied out of guest memory. Copied rather than pointed at because the
    /// handler runs on the calling thread and may block.
    std::vector<std::uint8_t> data;
    std::vector<IpcBuffer> buffers;
};

struct IpcResponse {
    std::uint32_t result = 0;
    std::vector<std::uint8_t> data;
};

/// Returned in a response when a service does not know the command. Nonzero, for
/// the same reason an SVC without a handler is nonzero.
inline constexpr std::uint32_t kIpcResultUnknownCommand = 0x5A1D'0002u;

enum class IpcError {
    None,
    /// The header itself could not be read.
    HeaderUnreadable,
    BadMagic,
    /// A length the guest gave is past what this layer will accept.
    TooLarge,
    TooManyBuffers,
    /// The header was readable but the data behind it was not.
    DataUnreadable,
    BufferTableUnreadable,
    /// A buffer the guest named is not reachable.
    BufferUnreachable,
    /// The response does not fit where the guest asked for it.
    ResponseTooLarge,
};

const char* ToString(IpcError error) noexcept;

/// Pull a request out of guest memory. On failure `request` is left empty.
bool UnpackRequest(cpu::GuestMemory& memory, std::uint64_t address, IpcRequest& request,
                   IpcError* error = nullptr);

/// Write a response into `room` bytes at `address`. Refuses rather than
/// truncating: a truncated response is a lie about what the service said.
bool PackResponse(cpu::GuestMemory& memory, std::uint64_t address, std::uint64_t room,
                  const IpcResponse& response, IpcError* error = nullptr);

/// The bytes a response of this size needs.
constexpr std::uint64_t ResponseBytes(std::uint64_t data_size) noexcept {
    return kIpcHeaderBytes + data_size;
}

}  // namespace zlong::service
