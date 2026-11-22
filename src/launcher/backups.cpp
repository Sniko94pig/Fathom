#include "launcher/backups.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

namespace launcher::backups {

namespace {

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string When(int y, int mo, int d, int h, int mi) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d %s %d, %02d:%02d", d, months[std::clamp(mo, 1, 12) - 1], y, h, mi);
    return buf;
}

std::string Size(unsigned long long b) {
    char buf[32];
    if (b >= 1024ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ull * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024));
    else std::snprintf(buf, sizeof(buf), "%llu KB", (b + 1023) / 1024);
    return buf;
}

std::string GameFor(const std::string& prefix, std::string& note) {
    if (prefix.empty()) return "Subnautica 2";
    if (prefix == "Subnautica") return "Subnautica";
    if (prefix == "SonsOfTheForest") return "Sons of the Forest";
    if (prefix == "ULTRAKILL") return "ULTRAKILL";
    if (prefix == "GeometryDash") return "Geometry Dash";
    if (prefix.rfind("Revive", 0) == 0) {
        note = "Before a revive";
        return "Sons of the Forest";
    }
    std::string g = prefix;
    std::replace(g.begin(), g.end(), '_', ' ');
    return g;
}

bool Inside(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::weakly_canonical(root, ec), b = fs::weakly_canonical(p, ec);
    return !a.empty() && b.parent_path() == a;
}

}

std::vector<ui::BackupEntry> List(const std::vector<Root>& roots) {
    static const std::wregex stampRe(LR"(^(.*?)_?(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{2})(?:-\d{1,3})?(?:[ _]\(?\d+\)?)?$)");
    std::vector<ui::BackupEntry> out;
    for (const Root& r : roots) {
        std::error_code ec;
        if (r.path.empty() || !fs::is_directory(r.path, ec)) continue;
        for (const auto& e : fs::directory_iterator(r.path, ec)) {
            std::error_code ec2;
            if (!e.is_directory(ec2)) continue;
            ui::BackupEntry b;
            b.path = e.path().wstring();
            std::wstring name = e.path().filename().wstring();
            std::wsmatch m;
            if (std::regex_match(name, m, stampRe)) {
                int y = std::stoi(m[2]), mo = std::stoi(m[3]), d = std::stoi(m[4]), h = std::stoi(m[5]), mi = std::stoi(m[6]), s = std::stoi(m[7]);
                b.game = r.game ? r.game : GameFor(Narrow(m[1]), b.note);
                b.when = When(y, mo, d, h, mi);
                b.sortKey = ((((long long)y * 100 + mo) * 100 + d) * 100 + h) * 10000 + mi * 100 + s;
            } else {
                auto ft = fs::last_write_time(e.path(), ec2);
                auto st = std::chrono::clock_cast<std::chrono::system_clock>(ft);
                std::time_t tt = std::chrono::system_clock::to_time_t(st);
                std::tm tm{};
                localtime_s(&tm, &tt);
                b.game = r.game ? r.game : "Other";
                b.note = Narrow(name);
                b.when = When(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
                b.sortKey = ((((long long)(tm.tm_year + 1900) * 100 + tm.tm_mon + 1) * 100 + tm.tm_mday) * 100 + tm.tm_hour) * 10000 +
                            tm.tm_min * 100 + tm.tm_sec;
            }
            unsigned long long bytes = 0;
            for (auto it = fs::recursive_directory_iterator(e.path(), fs::directory_options::skip_permission_denied, ec2);
                 !ec2 && it != fs::recursive_directory_iterator(); it.increment(ec2)) {
                std::error_code ec3;
                if (it->is_regular_file(ec3)) bytes += it->file_size(ec3);
            }
            b.bytes = bytes;
            b.size = Size(bytes);
            out.push_back(std::move(b));
        }
    }
    std::sort(out.begin(), out.end(), [](const ui::BackupEntry& a, const ui::BackupEntry& b) {
        if (a.game != b.game) return a.game < b.game;
        return a.sortKey > b.sortKey;
    });
    return out;
}

std::string SizeText(unsigned long long bytes) { return Size(bytes); }

int ScanOld(const std::vector<std::wstring>& folders, unsigned long long& bytes) {
    bytes = 0;
    int count = 0;
    for (const std::wstring& f : folders) {
        std::error_code ec;
        if (f.empty() || !fs::is_directory(f, ec)) continue;
        for (const auto& e : fs::directory_iterator(f, ec)) {
            std::error_code ec2;
            if (e.is_directory(ec2)) ++count;
            else if (e.is_regular_file(ec2)) {
                bytes += e.file_size(ec2);
                continue;
            }
            for (auto it = fs::recursive_directory_iterator(e.path(), fs::directory_options::skip_permission_denied, ec2);
                 !ec2 && it != fs::recursive_directory_iterator(); it.increment(ec2)) {
                std::error_code ec3;
                if (it->is_regular_file(ec3)) bytes += it->file_size(ec3);
            }
        }
    }
    return count;
}

int DeleteOld(const std::vector<std::wstring>& folders) {
    int failed = 0;
    for (const std::wstring& f : folders) {
        std::error_code ec;
        if (f.empty() || !fs::is_directory(f, ec)) continue;
        for (const auto& e : fs::directory_iterator(f, ec)) {
            std::error_code ec2;
            fs::remove_all(e.path(), ec2);
            if (ec2) ++failed;
        }
    }
    return failed;
}

int Recycle(const std::vector<Root>& roots, const std::vector<std::wstring>& paths) {
    std::wstring list;
    int n = 0;
    for (const std::wstring& p : paths) {
        bool ok = false;
        for (const Root& r : roots)
            if (!r.path.empty() && Inside(r.path, p)) ok = true;
        std::error_code ec;
        if (!ok || !fs::is_directory(p, ec)) continue;
        list += fs::path(p).wstring();
        list.push_back(L'\0');
        ++n;
    }
    if (!n) return 0;
    list.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_NOERRORUI | FOF_SILENT;
    int r = SHFileOperationW(&op);
    if (r != 0 || op.fAnyOperationsAborted) return -1;
    return n;
}

}
