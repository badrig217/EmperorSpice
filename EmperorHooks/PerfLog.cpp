#include "PerfLog.hpp"
#include "GameMessage.hpp"
#include "Log.hpp"
#include <windows.h>
#include <cstdint>
#include <cstdio>

// Frame times are measured between two EndScene calls. The game is capped at 60 fps by HookD3D7, so a
// normal frame takes about 17 ms. Anything well above that is felt as a stutter.
namespace
{
  bool gEnabled = false;
  LARGE_INTEGER gFreq = {};
  LARGE_INTEGER gLastFrame = {};
  LARGE_INTEGER gWindowStart = {};

  // per summary window
  int gFrames = 0;
  double gWorstMs = 0;
  int gHitches = 0; // > 50 ms: a visible jerk
  int gStalls = 0;  // > 100 ms: a clear hang
  int gStallLinesThisWindow = 0;
  bool gMarkerDown = false;
  bool gWasInMission = false;

  const double kHitchMs = 50.0;
  const double kStallMs = 100.0;
  const double kSummarySeconds = 30.0;
  const int kMaxStallLinesPerWindow = 20; // keep the log readable when something hangs for a long time

  // != 0 while a battlefield is running (see PickingTracer.cpp). Overridable for the frame-time test.
#ifndef PERFLOG_MISSION_FLAG
#define PERFLOG_MISSION_FLAG 0x00817c0c
#endif
  // a pointer (HUD object), not a byte flag: read the whole dword
  bool inMission() { return *reinterpret_cast<volatile uintptr_t*>(PERFLOG_MISSION_FLAG) != 0; }

  bool gameWindowFocused()
  {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg)
      GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
  }

  double ms(int64_t ticks) { return double(ticks) * 1000.0 / double(gFreq.QuadPart); }

  void clock(char* out, size_t size)
  {
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, size, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
  }

  void resetWindow(const LARGE_INTEGER& now)
  {
    gWindowStart = now;
    gFrames = 0;
    gWorstMs = 0;
    gHitches = 0;
    gStalls = 0;
    gStallLinesThisWindow = 0;
  }
}

void initPerfLog(bool enabled)
{
  gEnabled = enabled;
  QueryPerformanceFrequency(&gFreq);
  Log("PerfLog: %s (slow frames > %.0f ms and a summary every %.0f s while in a mission; Ctrl+Shift+L = marker)\n",
      enabled ? "on" : "off", kStallMs, kSummarySeconds);
}

void perfLogFrame()
{
  if (!gEnabled)
    return;

  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const bool mission = inMission();
  char t[16];

  if (mission != gWasInMission)
  {
    clock(t, sizeof(t));
    Log("PerfLog %s: %s\n", t, mission ? "mission started" : "mission ended");
    gWasInMission = mission;
    gLastFrame.QuadPart = 0; // loading a mission is a long "frame" - don't count it as a stutter
    resetWindow(now);
  }

  // marker: Ctrl+Shift+L, once per press, only when the game has the focus
  bool down = gameWindowFocused() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
              (GetAsyncKeyState('L') & 0x8000);
  if (down && !gMarkerDown)
  {
    clock(t, sizeof(t));
    Log("PerfLog %s: ---- MARKER (Ctrl+Shift+L) ----\n", t);
    showGameMessage(gameMessageGerman() ? "Debug-Log: Marke gesetzt" : "Debug log: marker set");
  }
  gMarkerDown = down;

  if (gLastFrame.QuadPart != 0 && mission)
  {
    double frameMs = ms(now.QuadPart - gLastFrame.QuadPart);
    gFrames++;
    if (frameMs > gWorstMs) gWorstMs = frameMs;
    if (frameMs > kHitchMs) gHitches++;
    if (frameMs > kStallMs)
    {
      gStalls++;
      if (gStallLinesThisWindow < kMaxStallLinesPerWindow)
      {
        clock(t, sizeof(t));
        // in the background (Alt+Tab) slow frames are normal - say so, so they can be told apart
        Log("PerfLog %s: slow frame %.0f ms%s\n", t, frameMs, gameWindowFocused() ? "" : " (game in background)");
        if (++gStallLinesThisWindow == kMaxStallLinesPerWindow)
          Log("PerfLog %s: (more slow frames in this %.0f s window are only counted)\n", t, kSummarySeconds);
      }
    }
  }
  gLastFrame = now;

  double windowSeconds = ms(now.QuadPart - gWindowStart.QuadPart) / 1000.0;
  if (windowSeconds >= kSummarySeconds)
  {
    if (mission && gFrames > 0)
    {
      clock(t, sizeof(t));
      Log("PerfLog %s: %.0f s: %.1f fps, slowest frame %.0f ms, %d jerks (> %.0f ms), %d hangs (> %.0f ms)\n", t, windowSeconds,
          gFrames / windowSeconds, gWorstMs, gHitches, kHitchMs, gStalls, kStallMs);
    }
    resetWindow(now);
  }
}
