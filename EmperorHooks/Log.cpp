#include "Log.hpp"
#include <cstdio>

// emperor.txt in the game's data folder. Optional (launcher: Troubleshooting > "Log file"); when it is off
// nothing is written to disk and an old emperor.txt is removed.
static bool gLogToFile = true;

void setLogToFile(bool enabled)
{
  gLogToFile = enabled;
  if (!enabled)
    remove("emperor.txt");
}

void Log(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  vaLog(format, args);
  va_end(args);
}

void vaLog(const char* format, va_list args)
{
  va_list copy;
  va_copy(copy, args);
  vprintf(format, copy);
  va_end(copy);

  if (!gLogToFile)
    return;
  static FILE* f = fopen("emperor.txt", "wb");
  if (!f)
    return;
  vfprintf(f, format, args);
  fflush(f);
}