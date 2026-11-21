#pragma once
#include <string>

namespace slog {

void Open(const std::wstring& path);
void Write(const char* fmt, ...);
void Close();

}

#define LOG(...) slog::Write(__VA_ARGS__)
