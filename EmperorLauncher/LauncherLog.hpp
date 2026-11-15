#pragma once
// launcher.txt in the game folder (next to emperor.txt): what the launcher did - start, applying the
// graphics pack / fonts / mods / HD textures, the graphics workshop, starting the game. Only with the
// debug log on (Troubleshooting > "Debug log"); otherwise nothing is written and an old file is removed.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace launcherLog
{
  inline std::mutex& lock() { static std::mutex m; return m; }
  inline FILE*& file() { static FILE* f = nullptr; return f; }
  inline bool& enabled() { static bool e = false; return e; }

  // Call once the game folder and the setting are known. Starts a new file each launcher start.
  inline void open(const std::filesystem::path& gameDir, bool on)
  {
    std::lock_guard<std::mutex> g(lock());
    if (file()) { fclose(file()); file() = nullptr; }
    enabled() = on;
    std::filesystem::path p = gameDir / L"launcher.txt";
    if (!on) { std::error_code ec; std::filesystem::remove(p, ec); return; }
    file() = _wfopen(p.c_str(), L"wb");
  }

  inline void write(const char* format, ...)
  {
    std::lock_guard<std::mutex> g(lock());
    if (!enabled() || !file()) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(file(), "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, format);
    vfprintf(file(), format, a);
    va_end(a);
    fputc('\n', file());
    fflush(file());
  }

  // UTF-8 for paths and messages
  inline std::string utf8(const std::wstring& w)
  {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
  }
}
