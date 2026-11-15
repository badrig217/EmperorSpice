#include "Log.hpp"
#include <windows.h>
#include <cstdio>

// emperor.txt in the game's data folder ("Debug log" in the launcher). When it is off nothing is
// written to disk and an old emperor.txt is removed. Every line starts with the local time.
static bool gLogToFile = true;
static FILE* gFile = nullptr;
static bool gAtLineStart = true;
static volatile DWORD gLastWriteTick = 0;
static CRITICAL_SECTION gLock;
static bool gLockReady = false;

static void ensureLock()
{
  // Log() is first called from the main thread before any other thread of ours exists.
  if (!gLockReady)
  {
    InitializeCriticalSection(&gLock);
    gLockReady = true;
  }
}

void setLogToFile(bool enabled)
{
  ensureLock();
  gLogToFile = enabled;
  if (!enabled)
    remove("emperor.txt");
}

unsigned long logLastWriteTick() { return gLastWriteTick; }

void Log(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  vaLog(format, args);
  va_end(args);
}

void vaLog(const char* format, va_list args)
{
  ensureLock();
  char buffer[2048];
  va_list copy;
  va_copy(copy, args);
  int n = vsnprintf(buffer, sizeof(buffer), format, copy);
  va_end(copy);
  if (n < 0)
    return;
  if (n >= int(sizeof(buffer)))
    n = int(sizeof(buffer)) - 1;

  EnterCriticalSection(&gLock);
  fwrite(buffer, 1, size_t(n), stdout);
  gLastWriteTick = GetTickCount();
  if (gLogToFile)
  {
    if (!gFile)
      gFile = fopen("emperor.txt", "wb");
    if (gFile)
    {
      SYSTEMTIME t;
      GetLocalTime(&t);
      char stamp[24];
      snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
      const char* p = buffer;
      const char* end = buffer + n;
      while (p < end)
      {
        if (gAtLineStart)
          fputs(stamp, gFile);
        const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
        const char* stop = nl ? nl + 1 : end;
        fwrite(p, 1, size_t(stop - p), gFile);
        gAtLineStart = nl != nullptr;
        p = stop;
      }
      fflush(gFile);
    }
  }
  LeaveCriticalSection(&gLock);
}