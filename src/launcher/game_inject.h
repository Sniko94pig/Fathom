#pragma once
#include "launcher/inject.h"
#include <filesystem>
#include <string>

namespace gameinject {

struct Spec {
    const wchar_t* exe;
    const wchar_t* steamDir;
    const wchar_t* dll;
    const wchar_t* run;
    const char* name;
    int appId;
    const wchar_t* backupName;
    std::filesystem::path (*saves)(const inject::Paths& p);
};

extern const Spec kSubnautica;
extern const Spec kSonsOfTheForest;
extern const Spec kUltrakill;

inject::Paths Locate(const Spec& s);
inject::GameState State(const Spec& s);
std::string Inject(const Spec& s, const inject::Paths& p, bool& ok, const std::string& openKey);
void StartGame(const Spec& s);
std::string BackupSaves(const Spec& s, const inject::Paths& p);

}
