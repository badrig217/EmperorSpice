#pragma once
// Optional "stutter log" (launcher: Troubleshooting > "Stutter log"). Measures the time between rendered
// frames and writes slow frames plus a short summary to data\emperor.txt, so stutters can be matched to
// what the player was doing. Ctrl+Shift+L writes a marker line ("I just gave an order to many units").
void initPerfLog(bool enabled);
void perfLogFrame(); // call once per rendered frame (from EndScene, before the frame limiter)
