#pragma once
#include <cstdint>

// Shared read-only game state (Game.exe 1.09, see docs/reverse-engineering.md).
namespace gameState
{
  // [0x817c0c] is the battlefield sidebar/HUD object: a POINTER, non-null while a battlefield runs.
  // Always read the whole dword - the low byte of a heap pointer can be 0.
  inline bool inMission() { return *reinterpret_cast<volatile uintptr_t*>(0x00817c0c) != 0; }

  // [0x7cb9d4] command/network manager (new object per game), +0x14 = game frame counter of that game.
  inline uintptr_t netManager() { return *reinterpret_cast<volatile uintptr_t*>(0x007cb9d4); }
  inline int gameFrame()
  {
    uintptr_t net = netManager();
    return net ? *reinterpret_cast<volatile int*>(net + 0x14) : -1;
  }
}
