#pragma once
// Diagnostic: logs every texture the game loads until the first mission starts (and at most a few
// thousand lines), so a hang while loading can be pinned to one file. Only with the log file on.
void initTextureTrace(bool enabled);
