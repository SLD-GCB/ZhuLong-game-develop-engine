#include "zlong/engine/file_io.h"

#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace zlong::engine {

namespace {

#if defined(_WIN32)

/// UTF-8 的一串字节，变成这个平台的宽字符。**转不动就返回空的**，让调用方去报"开不了" ——
/// 那串字节根本不是 UTF-8 的时候，能做的只有不猜。
std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const auto length = static_cast<int>(utf8.size());
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, wide.data(), needed) != needed) {
        return {};
    }
    return wide;
}

#endif

}  // namespace

std::FILE* OpenForRead(const std::string& utf8_path) {
#if defined(_WIN32)
    const std::wstring wide = Widen(utf8_path);
    return wide.empty() ? nullptr : ::_wfopen(wide.c_str(), L"rb");
#else
    return std::fopen(utf8_path.c_str(), "rb");
#endif
}

std::FILE* OpenForWrite(const std::string& utf8_path) {
#if defined(_WIN32)
    const std::wstring wide = Widen(utf8_path);
    return wide.empty() ? nullptr : ::_wfopen(wide.c_str(), L"wb");
#else
    return std::fopen(utf8_path.c_str(), "wb");
#endif
}

bool ReadWholeFile(const std::string& utf8_path, std::string& contents) {
    std::FILE* file = OpenForRead(utf8_path);
    if (file == nullptr) {
        return false;
    }
    std::string gathered;
    char block[65536];
    std::size_t got = 0;
    while ((got = std::fread(block, 1, sizeof(block), file)) > 0) {
        gathered.append(block, got);
    }
    const bool clean = std::ferror(file) == 0;
    std::fclose(file);
    if (!clean) {
        return false;
    }
    contents = std::move(gathered);
    return true;
}

bool WriteWholeFile(const std::string& utf8_path, const std::string& contents) {
    std::FILE* file = OpenForWrite(utf8_path);
    if (file == nullptr) {
        return false;
    }
    const std::size_t written = contents.empty()
                                    ? 0
                                    : std::fwrite(contents.data(), 1, contents.size(), file);
    // `fclose` 会冲刷，所以它返回什么也要看 —— 短写和冲刷失败都可能只在最后才露出来。
    const bool flushed = std::fclose(file) == 0;
    return written == contents.size() && flushed;
}

}  // namespace zlong::engine
