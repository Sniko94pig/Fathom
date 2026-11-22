#pragma once
#include "ui/loader.h"
#include <string>
#include <vector>

namespace launcher::backups {

struct Root {
    std::wstring path;
    const char* game = nullptr;
};

std::vector<ui::BackupEntry> List(const std::vector<Root>& roots);
int Recycle(const std::vector<Root>& roots, const std::vector<std::wstring>& paths);
std::string SizeText(unsigned long long bytes);
int ScanOld(const std::vector<std::wstring>& folders, unsigned long long& bytes);
int DeleteOld(const std::vector<std::wstring>& folders);

}
