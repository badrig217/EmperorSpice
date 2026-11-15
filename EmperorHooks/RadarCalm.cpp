#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <detours.h>
#include "RadarCalm.hpp"
#include "GameState.hpp"
#include "Log.hpp"
#ifdef _MSC_VER
#include <intrin.h>
#define CALLER_ADDRESS() reinterpret_cast<uintptr_t>(_ReturnAddress())
#else
#define CALLER_ADDRESS() reinterpret_cast<uintptr_t>(__builtin_return_address(0))
#endif

// Game.exe 1.09 (see docs/reverse-engineering.md, "Radar"):
//   0x51a340  Visibility(side, int* tileXY) (thiscall on world+0xa270, ret 8) -> 0/1 visible, 2 fog
//             (explored, not in sight), 3 shroud. Called by the radar tile colour function 0x51a030 twice
//             (0x51a061: shroud check, 0x51a0b4: fog check -> darker colour, no unit dots) and once by
//             unit code (0x564a06). Only the radar's fog check is changed here; the unit code and with it
//             the game simulation always get the real value.
namespace
{
  const uintptr_t kVisVa = 0x0051a340;
  const uint8_t kVisOrig[] = { 0x8B, 0x54, 0x24, 0x08, 0x56, 0x8B, 0x42, 0x04 };
  const uintptr_t kRadarFogCheckReturn = 0x0051a0b9;
  const DWORD kHoldMs = 2500; // a tile that was fog must be visible this long before the radar shows it

  typedef int(__fastcall* Vis_t)(void* self, void* edx, int side, int* tile);
  Vis_t realVis = reinterpret_cast<Vis_t>(kVisVa);

  std::vector<DWORD> gLastFog; // per tile: last time the radar saw it in fog (0 = never)
  int gW = 0, gH = 0;
  uintptr_t gHud = 0;          // mission (HUD object) the table belongs to

  int __fastcall myVis(void* self, void* edx, int side, int* tile)
  {
    int state = realVis(self, edx, side, tile);
    if (CALLER_ADDRESS() != kRadarFogCheckReturn || state == 3 || !tile)
      return state;

    uintptr_t hud = *reinterpret_cast<volatile uintptr_t*>(0x00817c0c);
    uintptr_t world = *reinterpret_cast<volatile uintptr_t*>(0x00817c00);
    if (!hud || !world)
      return state;
    int w = *reinterpret_cast<int*>(world + 0xa270), h = *reinterpret_cast<int*>(world + 0xa274);
    if (hud != gHud || w != gW || h != gH)
    {
      if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
        return state;
      gHud = hud, gW = w, gH = h;
      gLastFog.assign(size_t(w) * h, 0);
    }
    int x = tile[0], y = tile[1];
    if (x < 0 || y < 0 || x >= gW || y >= gH)
      return state;
    DWORD now = GetTickCount();
    DWORD& lastFog = gLastFog[size_t(y) * gW + x];
    if (state == 2)
    {
      lastFog = now ? now : 1;
      return 2;
    }
    if (lastFog && now - lastFog < kHoldMs)
      return 2; // visible again only briefly: keep showing fog (never more than the player sees)
    return state;
  }
}

void initRadarCalm(bool enabled)
{
  if (!enabled)
  {
    Log("RadarCalm: off\n");
    return;
  }
  if (memcmp(reinterpret_cast<const void*>(kVisVa), kVisOrig, sizeof(kVisOrig)) != 0)
  {
    Log("RadarCalm: unexpected code at %08lX, not hooking\n", (unsigned long)kVisVa);
    return;
  }
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)realVis, reinterpret_cast<PVOID>(myVis));
  LONG e = DetourTransactionCommit();
  Log("RadarCalm: %s (fog at the edge of sight held %lu ms on the radar)\n", e == NO_ERROR ? "on" : "hook FAILED",
      (unsigned long)kHoldMs);
}
