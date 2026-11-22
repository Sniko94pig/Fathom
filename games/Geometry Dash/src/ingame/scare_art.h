#pragma once
#include <cstdint>
#include <vector>

namespace scare_art {

constexpr int kFaces = 3;
constexpr int kScreams = 3;

void Face(int variant, int size, std::vector<uint8_t>& rgba);
void Scream(int variant, std::vector<uint8_t>& wav);

}
