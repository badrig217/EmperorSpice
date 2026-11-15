#pragma once
// Debug log (launcher: Troubleshooting > "Debug log"): system profile, crash report + minidump, hang
// watchdog and memory use in emperor.txt. See DebugLog.cpp.
void initDebugLog(bool enabled);
void debugLogFrame(); // call once per rendered frame (EndScene)
