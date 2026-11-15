#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <unordered_map>
#include <detours.h>
#include "StuckLog.hpp"
#include "Log.hpp"

// Game.exe 1.09 (see docs/reverse-engineering.md, "Units and movement"):
//   0x562f00  unit move step (thiscall, 1 stack arg, ret 4). Called every tick by every unit state that
//             walks (move, attack-move, harvest, ...). Returns 0 = arrived, 1/2/3/... = still underway or
//             no way found. Hooked here only to watch: the call is passed through unchanged.
//             Units without an order (e.g. waiting at the rally point) do not call it at all.
//   unit +0x0c side, +0x14/+0x18/+0x1c position x/height/z (world units, 32 per tile), +0x38 state,
//        +0x70 type (+8 = type name, inline chars), +0xfc destination (vec3),
//        +0x140 path (64 x {u8 tileX, u8 tileY}), +0x1c0 path index, +0x1c4 path length.
//   [0x817c00] world: +0xa270 map width, +0xa274 height, +0xa278 tiles (8 bytes: u8 low nibble = object
//        count, +4 int16* object indices); row start of tile row y = [0x880058 + y*4].
//   [0xb7d130] object table, 16 bytes per entry, +4 = object pointer.
//   [0x7d75d4] setup, 0x5356a0 (thiscall) setup->GetPlayerSide().
// The hot path (every move step) only reads the unit the game just handed over. Everything else is read
// with ReadProcessMemory, so a stale pointer can never crash the game.
//
// Log lines (emperor.txt):
//   Stau: FEST <type> S<side> @x,y ->Ziel x,y | vor x,y: <what stands there> | hier: <...> | z<state> w<i>/<n> r<ret>
//     every object listed as <type> S<side> plus "steht" (no move order: idle, waiting, building),
//     "laeuft" (walking) or "FEST" (itself stuck); "steht" also covers a unit pausing between two tries
//   Stau: frei <type> S<side> @x,y nach Ns          got moving again
//   Stau: angekommen <type> S<side> @x,y nach Ns    reached its destination from the stuck spot
//   Stau: aufgegeben <type> S<side> @x,y nach Ns    stopped walking without getting through (gave up,
//                                                    new order or destroyed; written 10 s later)
//   Stau-Lage: every 10 s while anything is stuck: count, area, most blocked fields with their blockers
//   Stau-Minute: every minute: totals
//   Sync Tick <n>: <checksum> (<objects>)  every 150 game frames: checksum over every object on the map
//     (id and position), taken at the first move step of that frame. In a multiplayer game all machines
//     must log the same values for the same tick; the first differing tick shows when the games drifted
//     apart (compare the players' emperor.txt).
namespace
{
  const uintptr_t kMoveStepVa = 0x00562f00;
  const uint8_t kMoveStepOrig[] = { 0x6A, 0xFF, 0x68, 0x76, 0xC6, 0x5C, 0x00 };

  const DWORD kStuckMs = 8000;       // trying to move but no progress for this long -> report
  const float kMoveEps = 8.0f;       // a quarter tile counts as progress
  // No move step for this long -> unit stopped walking. Long on purpose: a blocked unit can pause between
  // tries (the move state waits 10-69 game ticks after "no way", 0x56061c) and must still count as stuck.
  const DWORD kForgetMs = 10000;
  const DWORD kActiveMs = 1500;      // move step within this time -> "laeuft" in the blocker list
  const DWORD kOverviewMs = 10000;   // situation report interval while units are stuck
  const int kReportsPerMinute = 150; // flood guard; the rest is only counted

  typedef int(__fastcall* MoveStep_t)(void* self, void* edx, int arg);
  MoveStep_t realMoveStep = reinterpret_cast<MoveStep_t>(kMoveStepVa);

  struct Track
  {
    float x = 0, z = 0;    // anchor position
    DWORD anchorTick = 0;  // when the unit was last seen making progress
    DWORD lastCall = 0;    // last move step
    int lastRet = 0;
    bool reported = false; // currently counted as stuck
    bool detailed = false; // got a FEST line (else it was over the flood limit)
    int nextX = -1, nextY = -1; // field it wanted to enter, at report time
  };
  std::unordered_map<uintptr_t, Track> tracks;

  DWORD minuteStart = 0, lastOverview = 0, lastCleanup = 0;
  int reportsThisMinute = 0, suppressedThisMinute = 0, freedThisMinute = 0, gaveUpThisMinute = 0;
  int playerSide = -2; // -2 = not asked yet
  int lastSyncFrame = -1;
  const int kSyncEvery = 150;

  int gameFrame()
  {
    uintptr_t net = *reinterpret_cast<uintptr_t volatile*>(0x007cb9d4);
    return net ? *reinterpret_cast<int volatile*>(net + 0x14) : -1;
  }

  // FNV-1a over all objects on the map tiles. Direct reads: called on the game thread inside a move step,
  // where the map and the objects are valid; every index and count is range-checked.
  void syncCheck(int frame)
  {
    uintptr_t world = *reinterpret_cast<uintptr_t*>(0x00817c00);
    uintptr_t table = *reinterpret_cast<uintptr_t*>(0x00b7d130);
    if (!world || !table)
      return;
    int w = *reinterpret_cast<int*>(world + 0xa270), h = *reinterpret_cast<int*>(world + 0xa274);
    uintptr_t tiles = *reinterpret_cast<uintptr_t*>(world + 0xa278);
    if (!tiles || w <= 0 || h <= 0 || w > 1024 || h > 1024)
      return;
    uint32_t hash = 2166136261u;
    auto mix = [&](uint32_t v) {
      for (int i = 0; i < 4; i++) { hash ^= (v >> (i * 8)) & 0xff; hash *= 16777619u; }
    };
    int objects = 0;
    for (int y = 0; y < h; y++)
    {
      int rowStart = *reinterpret_cast<int*>(0x00880058 + uintptr_t(y) * 4);
      for (int x = 0; x < w; x++)
      {
        uintptr_t tile = tiles + uintptr_t(rowStart + x) * 8;
        int count = *reinterpret_cast<uint8_t*>(tile) & 0xf;
        const int16_t* idxs = *reinterpret_cast<int16_t**>(tile + 4);
        if (!count || !idxs)
          continue;
        for (int i = 0; i < count; i++)
        {
          int idx = idxs[i];
          if (idx < 0 || idx > 0x7fff) continue;
          uintptr_t obj = *reinterpret_cast<uintptr_t*>(table + uintptr_t(idx) * 16 + 4);
          if (!obj) continue;
          mix(uint32_t(x | (y << 16)));
          mix(*reinterpret_cast<uint32_t*>(obj + 4));    // object id
          mix(*reinterpret_cast<uint32_t*>(obj + 0x14)); // x (float bits)
          mix(*reinterpret_cast<uint32_t*>(obj + 0x1c)); // z
          objects++;
        }
      }
    }
    Log("Sync Tick %d: %08X (%d Objekte)\n", frame, hash, objects);
  }

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

  // "ATInfantry S1" (+ tile) for log lines
  void who(uintptr_t unit, const Track& t, char* buf, size_t size)
  {
    char name[48];
    int side = -1;
    typeName(unit, name, sizeof(name));
    rd(unit + 0x0c, side);
    snprintf(buf, size, "%s S%d @%d,%d", name, side, int(t.x / 32.0f), int(t.z / 32.0f));
  }

  const char* motion(uintptr_t obj, DWORD now)
  {
    auto it = tracks.find(obj);
    if (it == tracks.end() || now - it->second.lastCall > kForgetMs)
      return "steht";
    if (it->second.reported)
      return "FEST";
    return now - it->second.lastCall <= kActiveMs ? "laeuft" : "steht";
  }

  // "ATInfantry S1 steht, HKBuzzsaw S2 laeuft" for everything on tile x/y, or "leer"
  void tileObjects(int x, int y, uintptr_t self, DWORD now, char* out, size_t size)
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
      int n = obj == self ? snprintf(out + len, size - len, "%sselbst", len ? ", " : "")
                          : snprintf(out + len, size - len, "%s%s S%d %s", len ? ", " : "", name, side, motion(obj, now));
      if (n < 0) break;
      len += size_t(n) < size - len ? size_t(n) : size - len - 1;
    }
    if (!out[0])
      snprintf(out, size, "leer");
  }

  int getPlayerSide()
  {
    if (playerSide != -2)
      return playerSide;
    playerSide = -1;
    void* setup = nullptr;
    if (rd(0x007d75d4, setup) && setup)
      playerSide = reinterpret_cast<int(__thiscall*)(void*)>(0x005356a0)(setup);
    Log("Stau: Spielerseite = S%d\n", playerSide);
    return playerSide;
  }

  void nextField(uintptr_t unit, int& nx, int& ny)
  {
    int pathIdx = 0, pathLen = 0;
    float dx = 0, dz = 0;
    rd(unit + 0xfc, dx);
    rd(unit + 0x104, dz);
    nx = int(dx / 32.0f), ny = int(dz / 32.0f); // destination, unless a path point is pending
    if (rd(unit + 0x1c0, pathIdx) && rd(unit + 0x1c4, pathLen) && pathIdx >= 0 && pathIdx < pathLen && pathLen <= 64)
    {
      uint8_t p[2] = {};
      if (rd(unit + 0x140 + uintptr_t(pathIdx) * 2, p))
        nx = p[0], ny = p[1];
    }
  }

  void report(uintptr_t unit, Track& t, DWORD now)
  {
    nextField(unit, t.nextX, t.nextY);
    if (reportsThisMinute >= kReportsPerMinute)
    {
      suppressedThisMinute++;
      return;
    }
    reportsThisMinute++;
    t.detailed = true;
    getPlayerSide();
    char me[96], next[400], here[400];
    who(unit, t, me, sizeof(me));
    tileObjects(t.nextX, t.nextY, unit, now, next, sizeof(next));
    tileObjects(int(t.x / 32.0f), int(t.z / 32.0f), unit, now, here, sizeof(here));
    int state = 0, pathIdx = 0, pathLen = 0;
    float dx = 0, dz = 0;
    rd(unit + 0x38, state);
    rd(unit + 0x1c0, pathIdx);
    rd(unit + 0x1c4, pathLen);
    rd(unit + 0xfc, dx);
    rd(unit + 0x104, dz);
    Log("Stau: FEST %s ->Ziel %d,%d | vor %d,%d: %s | hier: %s | z%d w%d/%d r%d\n", me, int(dx / 32.0f),
        int(dz / 32.0f), t.nextX, t.nextY, next, here, state, pathIdx, pathLen, t.lastRet);
  }

  void ended(uintptr_t unit, const Track& t, const char* how, DWORD now)
  {
    if (!t.detailed) // over the flood limit: only counted
      return;
    char me[96];
    who(unit, t, me, sizeof(me));
    Log("Stau: %s %s nach %lus\n", how, me, (unsigned long)((now - t.anchorTick) / 1000));
  }

  // Every 10 s while anything is stuck: how many, where, and the fields most units wait for
  void overview(DWORD now)
  {
    if (now - lastOverview < kOverviewMs)
      return;
    lastOverview = now;
    int stuck = 0, walking = 0, minX = 9999, minY = 9999, maxX = -1, maxY = -1;
    std::map<std::pair<int, int>, int> waitingFor;
    uintptr_t sample = 0;
    for (auto& kv : tracks)
    {
      const Track& t = kv.second;
      if (now - t.lastCall > kForgetMs)
        continue;
      if (!t.reported)
      {
        walking++;
        continue;
      }
      stuck++;
      int x = int(t.x / 32.0f), y = int(t.z / 32.0f);
      minX = x < minX ? x : minX, minY = y < minY ? y : minY, maxX = x > maxX ? x : maxX, maxY = y > maxY ? y : maxY;
      waitingFor[{ t.nextX, t.nextY }]++;
      sample = kv.first;
    }
    if (!stuck)
      return;
    Log("Stau-Lage: %d fest, %d laufen, Bereich %d,%d bis %d,%d\n", stuck, walking, minX, minY, maxX, maxY);
    // the three fields most stuck units want to enter, with what blocks them
    for (int rank = 0; rank < 3 && !waitingFor.empty(); rank++)
    {
      auto best = waitingFor.begin();
      for (auto it = waitingFor.begin(); it != waitingFor.end(); ++it)
        if (it->second > best->second) best = it;
      char objs[400];
      tileObjects(best->first.first, best->first.second, sample, now, objs, sizeof(objs));
      Log("Stau-Lage:   Feld %d,%d wird von %d erwartet, dort: %s\n", best->first.first, best->first.second,
          best->second, objs);
      waitingFor.erase(best);
    }
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
    if (reportsThisMinute || suppressedThisMinute || freedThisMinute || gaveUpThisMinute)
      Log("Stau-Minute: %d neu fest (%d davon ohne Einzelzeile), %d wieder frei/angekommen, %d aufgegeben\n",
          reportsThisMinute + suppressedThisMinute, suppressedThisMinute, freedThisMinute, gaveUpThisMinute);
    minuteStart = now;
    reportsThisMinute = suppressedThisMinute = freedThisMinute = gaveUpThisMinute = 0;
  }

  void cleanup(DWORD now)
  {
    if (now - lastCleanup < 1000)
      return;
    lastCleanup = now;
    for (auto it = tracks.begin(); it != tracks.end();)
    {
      if (now - it->second.lastCall > kForgetMs) // not walking any more
      {
        if (it->second.reported)
        {
          // The unit may be gone, so only read it through rd() (inside who()).
          ended(it->first, it->second, "aufgegeben", now);
          gaveUpThisMinute++;
        }
        it = tracks.erase(it);
      }
      else
        ++it;
    }
  }

  int __fastcall myMoveStep(void* self, void* edx, int arg)
  {
    // Position before the step: the step could in theory remove the unit, so it is not touched afterwards
    // (one tick of lag does not matter for an 8 second threshold).
    int frame = gameFrame();
    if (frame < lastSyncFrame) // new game: frame counter restarted
      lastSyncFrame = -1;
    if (frame >= 0 && frame != lastSyncFrame && frame % kSyncEvery == 0)
    {
      lastSyncFrame = frame;
      syncCheck(frame);
    }
    uintptr_t unit = reinterpret_cast<uintptr_t>(self);
    const float* pos = reinterpret_cast<const float*>(unit + 0x14);
    float x = pos[0], z = pos[2];
    int ret = realMoveStep(self, edx, arg);
    DWORD now = GetTickCount();

    Track& t = tracks[unit];
    bool fresh = t.lastCall == 0 || now - t.lastCall > kForgetMs;
    if (fresh || std::fabs(x - t.x) > kMoveEps || std::fabs(z - t.z) > kMoveEps)
    {
      if (t.reported && !fresh)
      {
        ended(unit, t, "frei", now);
        freedThisMinute++;
      }
      t.x = x;
      t.z = z;
      t.anchorTick = now;
      t.reported = t.detailed = false;
    }
    t.lastCall = now;
    t.lastRet = ret;

    if (ret == 0) // arrived
    {
      if (t.reported)
      {
        ended(unit, t, "angekommen", now);
        freedThisMinute++;
      }
      t.anchorTick = now;
      t.reported = t.detailed = false;
    }
    else if (!t.reported && now - t.anchorTick >= kStuckMs)
    {
      t.reported = true;
      report(unit, t, now);
    }

    overview(now);
    minuteSummary(now);
    cleanup(now);
    return ret;
  }
}

void initStuckLog(bool enabled)
{
  if (!enabled) return;
  if (memcmp(reinterpret_cast<const void*>(kMoveStepVa), kMoveStepOrig, sizeof(kMoveStepOrig)) != 0 &&
      *reinterpret_cast<const uint8_t*>(kMoveStepVa) != 0xE9) // E9 = already detoured by GroupMove
  {
    Log("Stau: unexpected code at %08lX, not hooking\n", (unsigned long)kMoveStepVa);
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
