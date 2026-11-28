#include "ingame/guard.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cwctype>
#include <string>

namespace guard {

namespace {

std::wstring Lower(const wchar_t* s) {
    std::wstring out(s ? s : L"");
    for (wchar_t& c : out) c = (wchar_t)std::towlower(c);
    return out;
}

std::string Narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) out.push_back(c < 128 ? (char)c : '?');
    return out;
}

}

Report Check() {
    Report r;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        r.service = true;
        r.detail = "could not list running programs";
    } else {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            std::wstring name = Lower(pe.szExeFile);
            if (name.rfind(L"beservice", 0) == 0) {
                r.service = true;
                r.detail += "running: " + Narrow(pe.szExeFile) + "; ";
            } else if (name == L"gta5_enhanced_be.exe" || name == L"gta5_be.exe") {
                r.launcher = true;
                r.detail += "running: " + Narrow(pe.szExeFile) + "; ";
            }
        }
        CloseHandle(snap);
    }
    for (const wchar_t* dll : {L"BEClient_x64.dll", L"BEClient.dll"}) {
        if (GetModuleHandleW(dll)) {
            r.client = true;
            r.detail += "loaded in the game: " + Narrow(dll) + "; ";
        }
    }
    std::wstring cmd = Lower(GetCommandLineW());
    r.flag = cmd.find(L"-nobattleye") != std::wstring::npos;
    if (!r.flag) r.detail += "the game was not started with BattlEye off (no -nobattleye); ";
    return r;
}

bool Blocked(const Report& r) { return r.service || r.launcher || r.client || !r.flag; }

std::string Describe(const Report& r) {
    if (!Blocked(r)) return "BattlEye is off";
    std::string d = r.detail;
    while (!d.empty() && (d.back() == ' ' || d.back() == ';')) d.pop_back();
    return "BattlEye is on or was not turned off: " + d;
}

}
