#include "ingame/log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace glog {

namespace {
FILE* file = nullptr;
std::mutex lock;
long written = 0;
}

void Open(const std::wstring& path) {
    std::lock_guard<std::mutex> g(lock);
    if (file) return;
    std::wstring prev = path;
    size_t dot = prev.find_last_of(L'.');
    prev = (dot == std::wstring::npos ? prev : prev.substr(0, dot)) + L".prev.log";
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && (info.nFileSizeLow || info.nFileSizeHigh))
        MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
    written = 0;
}

void Write(const char* fmt, ...) {
    std::lock_guard<std::mutex> g(lock);
    if (!file || written > 16 * 1024 * 1024) return;
    written += 128;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    std::vfprintf(file, fmt, a);
    va_end(a);
    std::fputc('\n', file);
    std::fflush(file);
}

void Close() {
    std::lock_guard<std::mutex> g(lock);
    if (file) std::fclose(file);
    file = nullptr;
}

}
