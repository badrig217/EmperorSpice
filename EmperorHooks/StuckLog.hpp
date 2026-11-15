#pragma once
// Debug log: "Stau-Melder" (jam reporter). Writes a line to emperor.txt when a unit keeps trying to
// move but has not left its spot for a few seconds, with what stands on the tile it wants to enter,
// and another line when it gets free again. Read-only; only with the debug log on. See StuckLog.cpp.
void initStuckLog(bool enabled);
