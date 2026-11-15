#pragma once
// Short info text in the game's own message line (where "Construction complete" etc. appear), e.g.
// "Gruppenbewegung: AN". Only during a mission and only from the game thread (frame hook, game hooks).
void initGameMessage(bool german);
bool gameMessageGerman();
void showGameMessage(const char* text); // plain ASCII; also written to the log
