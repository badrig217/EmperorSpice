#pragma once
#include <cstdarg>

// Writes to the console and, unless switched off in the launcher, to emperor.txt.
void setLogToFile(bool enabled); // call before the first Log()
unsigned long logLastWriteTick(); // GetTickCount() of the last Log() call (hang watchdog)
void Log(const char* format, ...);
void vaLog(const char* format, va_list args);