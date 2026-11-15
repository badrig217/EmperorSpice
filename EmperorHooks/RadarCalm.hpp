#pragma once
// Calmer radar: at cliffs the original radar flickers, because tiles at the edge of sight keep switching
// between "fog" (drawn darker) and "visible" (fog regrows, sight is recalculated, height blocks sight).
// Here a tile that was in fog stays fog ON THE RADAR until it has been visible for a moment without a
// break. The radar therefore never shows more than the player sees (no enemy units in fog); new sight
// at the edge only shows up a little later. Display only - no effect on the game, safe in multiplayer.
void initRadarCalm(bool enabled);
