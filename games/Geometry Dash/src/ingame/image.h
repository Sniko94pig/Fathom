#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace image {

bool Load(const std::wstring& path, std::vector<uint8_t>& rgba, int& width, int& height);

}
