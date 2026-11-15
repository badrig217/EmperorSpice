#include <windows.h>
#include <cstdint>
#include <cstring>
#include "GameMessage.hpp"
#include "Log.hpp"
#include "GameState.hpp"

// Game.exe 1.09 (see docs/reverse-engineering.md, "In-game messages"):
//   [0x8aa5c4]  message manager (0 outside a mission)
//   0x4e9940    manager->Add(const char* text, int, int) (thiscall, ret 0xc). Copies the text into a new
//               message object. Used by the game for mission messages and hints; one caller formats
//               into a stack buffer first, so the text does not have to outlive the call.
namespace
{
  bool gGerman = false;
  const uintptr_t kAddVa = 0x004e9940;
  const uint8_t kAddOrig[] = { 0x8B, 0x44, 0x24, 0x0C, 0x56, 0x8B, 0xF1 }; // mov eax,[esp+0xc]; push esi; mov esi,ecx
  bool gCodeOk = false;

  using gameState::inMission;
}

void initGameMessage(bool german)
{
  gGerman = german;
  gCodeOk = memcmp(reinterpret_cast<const void*>(kAddVa), kAddOrig, sizeof(kAddOrig)) == 0;
  if (!gCodeOk)
    Log("GameMessage: unexpected code at %08lX, in-game info texts off\n", (unsigned long)kAddVa);
}

bool gameMessageGerman() { return gGerman; }

void showGameMessage(const char* text)
{
  Log("Info im Spiel: %s\n", text);
  if (!gCodeOk || !inMission())
    return;
  void* manager = *reinterpret_cast<void* volatile*>(0x008aa5c4);
  if (!manager)
    return;
  char buf[160];
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  reinterpret_cast<void(__thiscall*)(void*, const char*, int, int)>(kAddVa)(manager, buf, 0, 0);
}
