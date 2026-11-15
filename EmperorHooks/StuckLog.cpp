#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <detours.h>
#include "StuckLog.hpp"
#include "Log.hpp"

// Game.exe 1.09 (see docs/reverse-engineering.md, "Units and movement"):
//   0x562f00  unit move step (thiscall, 1 stack arg, ret 4). Called every tick by every unit state that
//             walks (move, attack-move, harvest, ...). Returns 0 = arrived, 1/2/3/... = still underway or
//             no way found. Hooked here only to watch: the call is passed through unchanged.
//   unit +0x0c side, +0x14/+0x18/+0x1c position x/height/z (world units, 32 per tile), +0x38 state,
//        +0x70 type (+8 = type name, inline chars), +0xfc destination (vec3),
//        +0x140 path (64 x {u8 tileX, u8 tileY}), +0x1c0 path index, +0x1c4 path length.
//   [0x817c00] world: +0xa270 map width, +0xa274 height, +0xa278 tiles (8 bytes: u8 low nibble = object
//        count, +4 int16* object indices); row start of tile row y = [0x880058 + y*4].
//   [0xb7d130] object table, 16 bytes per entry, +4 = object pointer.
//   [0x7d75d4] setup, 0x5356a0 (thiscall) setup->GetPlayerSide().
// The hot path (every move step) only reads the unit the game just handed over. Everything else is read
// with ReadProcessMemory, so a stale pointer can never crash the game.
namespace
{
  const uintptr_t kMoveStepVa = 0x00562f00;
  const uint8_t kMoveStepOrig[] = { 0x6A, 0xFF, 0x68, 0x76, 0xC6, 0x5C, 0x00 };

  const DWORD kStuckMs = 8000;      // trying to move but no progress for this long -> report
  const float kMoveEps = 8.0f;      // a quarter tile counts as progress
  const DWORD kForgetMs = 5000;     // no move step for this long -> unit no longer walking, forget it
  const int kReportsPerMinute = 40; // flood guard; the rest is only counted

  typedef int(__fastcall* MoveStep_t)(void* self, void* edx, int arg);
  MoveStep_t realMoveStep = reinterpret_cast<MoveStep_t>(kMoveStepVa);

  struct Track
  {
    float x = 0, z = 0;      // anchor position
    DWORD anchorTick = 0;    // when the unit was last seen making progress
    DWORD lastCall = 0;      // last move step
    int lastRet = 0;
    bool reported = false;
  };
  std::unordered_map<uintptr_t, Track> tracks;

  DWORD minuteStart = 0;
  int reportsThisMinute = 0, suppressedThisMinute = 0, freedThisMinute = 0;
  DWORD lastCleanup = 0;
  int playerSide = -2; // -2 = not asked yet

  template <class T> bool rd(uintptr_t addr, T& out)
  {
    SIZE_T n = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), &out, sizeof(T), &n) &&
           n == sizeof(T);
  }

  void typeName(uintptr_t obj, char* buf, size_t size)
  {
    uintptr_t type = 0;
    buf[0] = 0;
    if (!rd(obj + 0x70, type) || !type)
    {
      snprintf(buf, size, "?");
      return;
    }
    SIZE_T n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(type + 8), buf, size - 1, &n))
      n = 0;
    buf[n < size ? n : size - 1] = 0;
    for (size_t i = 0; buf[i]; i++)
      if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7e) { buf[i] = 0; break; }
    if (!buf[0]) snprintf(buf, size, "?");
  }

  // "ATInfantry(S1) HKBuzzsaw(S2)" for everything standing on tile x/y, or "frei"
  void tileObjects(int x, int y, uintptr_t self, char* out, size_t size)
  {
    out[0] = 0;
    uintptr_t world = 0, tiles = 0, table = 0;
    int w = 0, h = 0, rowStart = 0;
    if (!rd(0x00817c00, world) || !world || !rd(world + 0xa270, w) || !rd(world + 0xa274, h) ||
        !rd(world + 0xa278, tiles) || !rd(0x00b7d130, table) || x < 0 || y < 0 || x >= w || y >= h ||
        !rd(0x00880058 + uintptr_t(y) * 4, rowStart))
    {
      snprintf(out, size, "(Karte nicht lesbar)");
      return;
    }
    uintptr_t tile = tiles + uintptr_t(rowStart + x) * 8;
    uint8_t count = 0;
    uintptr_t idxs = 0;
    if (!rd(tile, count) || !rd(tile + 4, idxs))
    {
      snprintf(out, size, "(Feld nicht lesbar)");
      return;
    }
    count &= 0xf;
    size_t len = 0;
    for (int i = 0; i < count && len + 1 < size; i++)
    {
      int16_t idx = 0;
      uintptr_t obj = 0;
      int side = -1;
      if (!rd(idxs + uintptr_t(i) * 2, idx) || idx < 0 || !rd(table + uintptr_t(idx) * 16 + 4, obj) || !obj)
        continue;
      char name[48];
      typeName(obj, name, sizeof(name));
      rd(obj + 0x0c, side);
      int n = snprintf(out + len, size - len, "%s%s(S%d)%s", len ? " " : "", name, side, obj == self ? "[selbst]" : "");
      if (n < 0) break;
      len += size_t(n);
    }
    if (!out[0])
      snprintf(out, size, "frei");
  }

  int getPlayerSide()
  {
    if (playerSide != -2)
      return playerSide;
    playerSide = -1;
    void* setup = nullptr;
    if (rd(0x007d75d4, setup) && setup)
      playerSide = reinterpret_cast<int(__thiscall*)(void*)>(0x005356a0)(setup);
    Log("Stau-Melder: Spielerseite = S%d\n", playerSide);
    return playerSide;
  }

  void report(uintptr_t unit, const Track& t, DWORD now)
  {
    char name[48];
    typeName(unit, name, sizeof(name));
    int side = -1, state = 0, pathIdx = 0, pathLen = 0;
    float dx = 0, dz = 0;
    rd(unit + 0x0c, side);
    rd(unit + 0x38, state);
    rd(unit + 0xfc, dx);
    rd(unit + 0x104, dz);
    rd(unit + 0x1c0, pathIdx);
    rd(unit + 0x1c4, pathLen);
    int tx = int(t.x / 32.0f), tz = int(t.z / 32.0f);
    int nx = int(dx / 32.0f), nz = int(dz / 32.0f); // next field: next path point, else the destination
    if (pathIdx >= 0 && pathIdx < pathLen && pathLen <= 64)
    {
      uint8_t p[2] = {};
      if (rd(unit + 0x140 + uintptr_t(pathIdx) * 2, p))
        nx = p[0], nz = p[1];
    }
    char next[512];
    tileObjects(nx, nz, unit, next, sizeof(next));
    char here[512];
    tileObjects(tx, tz, unit, here, sizeof(here));
    getPlayerSide();
    Log("Stau-Melder: FEST %s(S%d) %p seit %lus auf Feld %d,%d -> Ziel %d,%d | naechstes Feld %d,%d: %s | "
        "auf eigenem Feld: %s | Zustand %d, Weg %d/%d, Rueckgabe %d\n",
        name, side, (void*)unit, (unsigned long)((now - t.anchorTick) / 1000), tx, tz, int(dx / 32.0f),
        int(dz / 32.0f), nx, nz, next, here, state, pathIdx, pathLen, t.lastRet);
  }

  void minuteSummary(DWORD now)
  {
    if (minuteStart == 0)
    {
      minuteStart = now;
      return;
    }
    if (now - minuteStart < 60000)
      return;
    int stuck = 0;
    for (auto& kv : tracks)
      if (kv.second.reported) stuck++;
    if (reportsThisMinute || suppressedThisMinute || freedThisMinute || stuck)
      Log("Stau-Melder: letzte Minute %d gemeldet, %d nicht einzeln gemeldet (Flutschutz), %d wieder frei; "
          "jetzt fest: %d, laufende Einheiten: %d\n",
          reportsThisMinute, suppressedThisMinute, freedThisMinute, stuck, (int)tracks.size());
    minuteStart = now;
    reportsThisMinute = suppressedThisMinute = freedThisMinute = 0;
  }

  void cleanup(DWORD now)
  {
    if (now - lastCleanup < 2000)
      return;
    lastCleanup = now;
    for (auto it = tracks.begin(); it != tracks.end();)
    {
      // Not walking any more (arrived, new order, died): drop it. A unit that was stuck and simply gave
      // up or got another order is not "free", so it only counts in the summary.
      if (now - it->second.lastCall > kForgetMs)
        it = tracks.erase(it);
      else
        ++it;
    }
  }

  int __fastcall myMoveStep(void* self, void* edx, int arg)
  {
    // Position before the step: the step could in theory remove the unit, so it is not touched afterwards
    // (one tick of lag does not matter for an 8 second threshold).
    uintptr_t unit = reinterpret_cast<uintptr_t>(self);
    const float* pos = reinterpret_cast<const float*>(unit + 0x14);
    float x = pos[0], z = pos[2];
    int ret = realMoveStep(self, edx, arg);
    DWORD now = GetTickCount();

    Track& t = tracks[unit];
    if (t.lastCall == 0 || now - t.lastCall > kForgetMs || std::fabs(x - t.x) > kMoveEps || std::fabs(z - t.z) > kMoveEps)
    {
      if (t.reported && t.lastCall != 0 && now - t.lastCall <= kForgetMs)
      {
        char name[48];
        typeName(unit, name, sizeof(name));
        Log("Stau-Melder: frei %s %p nach %lus\n", name, self, (unsigned long)((now - t.anchorTick) / 1000));
        freedThisMinute++;
      }
      t.x = x;
      t.z = z;
      t.anchorTick = now;
      t.reported = false;
    }
    t.lastCall = now;
    t.lastRet = ret;

    if (ret == 0) // arrived: nothing to watch until the next order
      t.anchorTick = now;
    else if (!t.reported && now - t.anchorTick >= kStuckMs)
    {
      t.reported = true;
      if (reportsThisMinute < kReportsPerMinute)
      {
        reportsThisMinute++;
        report(unit, t, now);
      }
      else
        suppressedThisMinute++;
    }

    minuteSummary(now);
    cleanup(now);
    return ret;
  }
}

void initStuckLog(bool enabled)
{
  if (!enabled) return;
  if (memcmp(reinterpret_cast<const void*>(kMoveStepVa), kMoveStepOrig, sizeof(kMoveStepOrig)) != 0)
  {
    Log("Stau-Melder: unexpected code at %08lX, not hooking\n", (unsigned long)kMoveStepVa);
    return;
  }
  tracks.reserve(1024);
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)realMoveStep, reinterpret_cast<PVOID>(myMoveStep));
  LONG e = DetourTransactionCommit();
  Log("Stau-Melder: %s (Einheiten, die %lus nicht vorankommen)\n", e == NO_ERROR ? "on" : "hook FAILED",
      (unsigned long)(kStuckMs / 1000));
}
