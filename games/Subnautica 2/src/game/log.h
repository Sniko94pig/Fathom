#pragma once
#include <string>

namespace glog {

void Open(const std::wstring& path);
void Write(const char* fmt, ...);
void Close();

}

#define LOG(...) glog::Write(__VA_ARGS__)
