#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // K32GetProcessMemoryInfo from kernel32, no psapi.lib needed
#endif
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "DebugLog.hpp"
#include "Log.hpp"

// Debug log (launcher: Troubleshooting > "Debug log"). Everything here only runs when it is on:
//  - a short system profile at the start (Windows, CPU, RAM, graphics card + driver, desktop mode,
//    all launcher settings, Game.exe and the archives in DATA with our markers),
//  - a crash report (exception, module + offset, registers, return addresses on the stack) and a small
//    minidump emperor_crash.dmp,
//  - a hang watchdog: no frame and no log line for 15 s -> where the main thread is working,
//  - memory use every 60 s and when a mission starts (the game is 32 bit without Large Address Aware).

namespace
{
  bool gEnabled = false;
  HANDLE gMainThread = nullptr;
  DWORD gMainThreadId = 0;
  volatile DWORD gLastFrameTick = 0;
  volatile LONG gFrames = 0;
  bool gWasInMission = false;
  LPTOP_LEVEL_EXCEPTION_FILTER gPreviousFilter = nullptr;

  const DWORD kHangMs = 15000;
  const DWORD kMemoryEveryMs = 60000;
  const int kMaxHangReports = 6;

#ifndef DEBUGLOG_MISSION_FLAG
#define DEBUGLOG_MISSION_FLAG 0x00817c0c // != 0 while a battlefield runs (see PickingTracer.cpp); overridable for tests
#endif
  bool inMission() { return *reinterpret_cast<volatile uint8_t*>(DEBUGLOG_MISSION_FLAG) != 0; }

  // "Game.exe+0x1234" / "dgVoodoo...dll+0x..." for an address, empty if it is in no module.
  std::string where(uintptr_t addr)
  {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &m) || !m)
      return "";
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    char out[MAX_PATH + 32];
    snprintf(out, sizeof(out), "%s+0x%lX", base, (unsigned long)(addr - reinterpret_cast<uintptr_t>(m)));
    return out;
  }

  bool isCode(uintptr_t addr)
  {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
  }

  // A return address follows a call instruction: E8 rel32, or FF /2 (indirect call, 2/3/6/7 bytes).
  bool isReturnAddress(uintptr_t v)
  {
    if (v < 0x10007 || !isCode(v) || !isCode(v - 7)) return false;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(v);
    if (b[-5] == 0xE8) return true;
    for (int len : { 2, 3, 6, 7 })
      if (b[-len] == 0xFF && (b[-len + 1] & 0x38) == 0x10) return true;
    return false;
  }

  // Return addresses on the stack, newest first: the call chain (best guess, no debug symbols).
  void logStack(const char* label, const uint32_t* words, size_t count)
  {
    int found = 0;
    uintptr_t last = 0;
    for (size_t i = 0; i < count && found < 24; i++)
    {
      uintptr_t v = words[i];
      if (v == last || !isReturnAddress(v)) continue;
      std::string w = where(v);
      if (w.empty()) continue;
      Log("%s   %s\n", label, w.c_str());
      last = v;
      found++;
    }
    if (!found) Log("%s   (no code addresses found on the stack)\n", label);
  }

  void logMemory(const char* reason)
  {
    PROCESS_MEMORY_COUNTERS pmc = {};
    pmc.cb = sizeof(pmc);
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)); // PagefileUsage = memory the game committed
    // address space: how much is free, and the largest free block (big allocations need one piece)
    uint64_t freeTotal = 0, largestFree = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));)
    {
      if (mbi.State == MEM_FREE)
      {
        freeTotal += mbi.RegionSize;
        if (mbi.RegionSize > largestFree) largestFree = mbi.RegionSize;
      }
      uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      if (next <= a) break;
      a = next;
    }
    Log("Memory (%s): in use %lu MB, address space free %lu MB, largest free block %lu MB\n", reason,
        (unsigned long)(pmc.PagefileUsage >> 20), (unsigned long)(freeTotal >> 20), (unsigned long)(largestFree >> 20));
  }

  std::string regString(HKEY root, const char* path, const char* value)
  {
    char buf[512] = {};
    DWORD size = sizeof(buf) - 1;
    if (RegGetValueA(root, path, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return "";
    return buf;
  }

  void logSystem()
  {
    Log("==== Emperor Spice debug log ====\n");
    std::string version = regString(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\LauncherCustomSettings", "LauncherVersion");
    Log("Launcher: %s\n", version.empty() ? "?" : version.c_str());

    // Windows (RtlGetVersion tells the truth, GetVersionEx lies to old programs) and Wine
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW*);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersion_t>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    OSVERSIONINFOW osv = {};
    osv.dwOSVersionInfoSize = sizeof(osv);
    if (rtlGetVersion) rtlGetVersion(&osv);
    typedef const char*(__cdecl * wine_get_version_t)();
    auto wine = reinterpret_cast<wine_get_version_t>(reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")));
    std::string display = regString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion");
    Log("Windows: %lu.%lu build %lu %s%s%s\n", osv.dwMajorVersion, osv.dwMinorVersion, osv.dwBuildNumber, display.c_str(),
        wine ? ", Wine " : "", wine ? wine() : "");

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    std::string cpu = regString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    Log("CPU: %s (%lu threads), RAM %lu MB\n", cpu.c_str(), si.dwNumberOfProcessors, (unsigned long)(ms.ullTotalPhys >> 20));

    DISPLAY_DEVICEA dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); i++, dd.cb = sizeof(dd))
    {
      if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
      // DeviceKey: \Registry\Machine\System\CurrentControlSet\Control\Video\{...}\0000
      std::string key = dd.DeviceKey, driver;
      const char* prefix = "\\Registry\\Machine\\";
      if (_strnicmp(key.c_str(), prefix, strlen(prefix)) == 0)
        driver = regString(HKEY_LOCAL_MACHINE, key.c_str() + strlen(prefix), "DriverVersion");
      DEVMODEA dm = {};
      dm.dmSize = sizeof(dm);
      EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm);
      Log("Display%s: %s, driver %s, desktop %lux%lu %lu Hz\n", (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "",
          dd.DeviceString, driver.empty() ? "?" : driver.c_str(), dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    }

    // all launcher settings, as stored
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\LauncherCustomSettings", 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
      std::string line;
      char name[128], data[256];
      for (DWORD i = 0;; i++)
      {
        DWORD nameLen = sizeof(name), dataLen = sizeof(data) - 1, type = 0;
        if (RegEnumValueA(k, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataLen) != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;
        data[dataLen] = 0;
        line += std::string(line.empty() ? "" : ", ") + name + "=" + data;
      }
      RegCloseKey(k);
      Log("Settings: %s\n", line.c_str());
    }

    // Game.exe and the archives the game will load (with our markers)
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA("Game.exe", GetFileExInfoStandard, &fa))
      Log("Game.exe: %lu bytes\n", fa.nFileSizeLow);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("DATA\\*.RFH", &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
      do
      {
        std::string rfh = std::string("DATA\\") + fd.cFileName, rfd = rfh.substr(0, rfh.size() - 1) + "D";
        WIN32_FILE_ATTRIBUTE_DATA da = {};
        GetFileAttributesExA(rfd.c_str(), GetFileExInfoStandard, &da);
        // look for "EmperorSpice\..." names in the index (mods, HD textures)
        std::string tags;
        FILE* f = fopen(rfh.c_str(), "rb");
        if (f)
        {
          std::vector<char> idx(fd.nFileSizeLow);
          size_t got = fread(idx.data(), 1, idx.size(), f);
          fclose(f);
          std::string s(idx.data(), got);
          if (s.find("EmperorSpice\\ModPack.txt") != std::string::npos) tags += " [mods]";
          if (s.find("EmperorSpice\\HDTextures.txt") != std::string::npos) tags += " [HD textures]";
          if (s.find("SmallHD") != std::string::npos || s.find("StandardHD") != std::string::npos) tags += " [HD fonts]";
        }
        Log("Archive %s: %lu KB%s\n", fd.cFileName, (unsigned long)(da.nFileSizeLow >> 10), tags.c_str());
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
    logMemory("start");
    Log("=================================\n");
  }

  LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep)
  {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) != 0)
      return EXCEPTION_CONTINUE_SEARCH; // reentered (e.g. through another handler that chains to us): stop here
    {
      const EXCEPTION_RECORD* er = ep->ExceptionRecord;
      const CONTEXT* c = ep->ContextRecord;
      uintptr_t addr = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
      std::string w = where(addr);
      Log("==== CRASH ====\n");
      Log("Exception 0x%08lX at %08lX (%s)%s\n", er->ExceptionCode, (unsigned long)addr, w.empty() ? "no module" : w.c_str(),
          GetCurrentThreadId() == gMainThreadId ? " in the main thread" : "");
      if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        Log("  %s address %08lX\n", er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
            (unsigned long)er->ExceptionInformation[1]);
      Log("  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX\n", c->Eax, c->Ebx, c->Ecx, c->Edx,
          c->Esi, c->Edi, c->Ebp, c->Esp);
      Log("  call chain (newest first, best guess):\n");
      std::vector<uint32_t> stack(1024);
      SIZE_T got = 0;
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t(c->Esp)), stack.data(), stack.size() * 4, &got);
      logStack(" ", stack.data(), got / 4);
      logMemory("crash");

      // small minidump next to emperor.txt, for a closer look
      typedef BOOL(WINAPI * MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
      HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
      auto writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDump_t>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))) : nullptr;
      if (writeDump)
      {
        HANDLE f = CreateFileA("emperor_crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
          struct { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; } info = { GetCurrentThreadId(), ep, FALSE };
          // MiniDumpWithIndirectlyReferencedMemory (0x40) | MiniDumpWithDataSegs (0x1): small, but with the stacks
          BOOL ok = writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, 0x41, &info, nullptr, nullptr);
          CloseHandle(f);
          Log("Crash dump: emperor_crash.dmp %s\n", ok ? "written" : "FAILED");
        }
      }
      Log("===============\n");
    }
    return gPreviousFilter ? gPreviousFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
  }

  // Captures where the main thread is right now. Nothing is logged while it is suspended (it might hold
  // the log lock).
  void reportHang(DWORD silentMs)
  {
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    std::vector<uint32_t> stack(1024);
    SIZE_T got = 0;
    bool ok = false;
    if (SuspendThread(gMainThread) != DWORD(-1))
    {
      ok = GetThreadContext(gMainThread, &c) != FALSE;
      if (ok)
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t(c.Esp)), stack.data(), stack.size() * 4, &got);
      ResumeThread(gMainThread);
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    Log("==== HANG? no frame and no log line for %lu s%s ====\n", silentMs / 1000,
        pid == GetCurrentProcessId() ? "" : " (game in background or minimized)");
    if (!ok) { Log("  could not read the main thread\n"); return; }
    std::string w = where(c.Eip);
    Log("  main thread at %08lX (%s)\n", c.Eip, w.empty() ? "no module" : w.c_str());
    Log("  call chain (newest first, best guess):\n");
    logStack(" ", stack.data(), got / 4);
    logMemory("hang");
  }

  DWORD WINAPI watchdog(void*)
  {
    DWORD lastMemory = GetTickCount();
    DWORD lastReport = 0;
    int reports = 0;
    for (;;)
    {
      Sleep(1000);
      DWORD now = GetTickCount();
      DWORD lastActivity = gLastFrameTick;
      DWORD lastLog = DWORD(logLastWriteTick());
      if (int(lastLog - lastActivity) > 0) lastActivity = lastLog;
      DWORD silent = now - lastActivity;
      if (lastActivity && silent >= kHangMs && (reports == 0 || now - lastReport >= 2 * kHangMs) && reports < kMaxHangReports)
      {
        reportHang(silent);
        reports++;
        lastReport = GetTickCount();
      }
      else if (silent < kHangMs)
        reports = 0; // running again: a later hang gets reported too
      if (now - lastMemory >= kMemoryEveryMs)
      {
        logMemory(inMission() ? "mission" : "menu");
        lastMemory = now;
      }
    }
  }
}

void initDebugLog(bool enabled)
{
  gEnabled = enabled;
  if (!enabled)
    return;
  gMainThreadId = GetCurrentThreadId();
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &gMainThread,
                  THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
  logSystem();
  gPreviousFilter = SetUnhandledExceptionFilter(crashFilter);
  HANDLE t = CreateThread(nullptr, 64 * 1024, watchdog, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  Log("DebugLog: on (crash report, hang watchdog after %lu s, memory every %lu s)\n", kHangMs / 1000, kMemoryEveryMs / 1000);
}

void debugLogFrame()
{
  if (!gEnabled)
    return;
  gLastFrameTick = GetTickCount();
  if (InterlockedIncrement(&gFrames) == 1)
  {
    // dgVoodoo, ReShade and the driver are loaded now and may have set their own crash handler
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(crashFilter);
    if (prev != crashFilter) gPreviousFilter = prev;
  }
  bool mission = inMission();
  if (mission && !gWasInMission)
    logMemory("mission start");
  gWasInMission = mission;
}
