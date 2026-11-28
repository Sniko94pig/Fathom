#pragma once
#include <string>

namespace guard {

struct Report {
    bool service = false;
    bool launcher = false;
    bool client = false;
    bool flag = false;
    std::string detail;
};

Report Check();
bool Blocked(const Report& r);
std::string Describe(const Report& r);
std::string GameVersion();
bool KnownVersion(const std::string& version);

}
