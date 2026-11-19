#pragma once
#include <string>
#include <vector>

namespace launcher {

class LogView {
public:
    void Open(const std::wstring& path, const std::string& title);
    void Close();
    bool IsOpen() const { return open; }
    void Draw();

private:
    void Reload(bool force);
    void Body(float x0, float y0, float x1, float y1);

    std::wstring path;
    std::string title;
    std::string file;
    std::vector<std::string> lines;
    bool open = false;
    bool missing = true;
    bool follow = true;
    bool jump = false;
    unsigned long long lastCheck = 0;
    unsigned long long stamp = 0;
    unsigned long long size = 0;
    double copiedAt = -10.0;
    double changedAt = -10.0;
};

}
