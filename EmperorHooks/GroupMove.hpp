#pragma once
// Group movement (single player, the player's own move orders):
//  - Formation: each unit drives to "click point + its offset from the group centre" instead of all
//    units aiming at the same point ("magic box": only when the target lies outside the group; very
//    spread out groups are pulled together).
//  - Arrive together: each unit drives only as fast as needed to arrive with the slowest-to-arrive unit
//    (at least half its own speed; stuck units do not count; aircraft are left alone).
// Ctrl+Shift+F switches it on/off in the game (info text in the game). With the debug log on, every group
// order is measured (arrival times) also when the feature is off, so on/off can be compared.
// See GroupMove.cpp.
void initGroupMove(bool enabled, bool debugLog);
void groupMoveUpdateFrame(); // call once per rendered frame
