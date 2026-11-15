#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>
#include <detours.h>
#include "GroupMove.hpp"
#include "GameMessage.hpp"
#include "Log.hpp"

// Game.exe 1.09 (see docs/reverse-engineering.md, "Orders, move groups and speed"):
//   0x4c3900  command executor (cdecl, packet). Every order goes through it, the player's too (via the
//             command queue 0x50a280). Packet: +8 type (0 = move), +0xc side, +0x14/+0x1c target x/z,
//             +0x30 unit count, +0x34 unit ids (int32, max 64).
//   0x4c47ff  inside the executor, per unit: "mov edx,[ecx]; call [edx+0x20]" = unit->ReceiveOrder(packet)
//             (thiscall, ret 4). Replaced by a call to orderThunk, which hands each unit a copy of the
//             packet with its own formation target. Every unit already gets the same packet there; the
//             game's move group (0x4abcc0) is created before that loop from the original packet.
//   0x564d30  Unit::GetSpeed (thiscall, float in st0). Capped for units of a group while it drives.
//   0x562f00  unit move step (thiscall, 1 arg, ret 4; 0 = arrived). Watched for positions and arrival.
//   0x59b350  world->FindObject(int* id) (thiscall on [0x817c00]).
//   0x7d75d4  setup: 0x5344e0 game type (3 = multiplayer), 0x5356a0 player side.
//   unit: +0x0c side, +0x14/+0x1c position x/z (32 world units per tile), +0x70 type, +0xfc/+0x104
//         destination x/z. type: +0 category (1 = unit), +0x3bd CanFly.
//   world: +0xa270/+0xa274 map width/height in tiles.
//   [0x7cb9d4] command/network manager, +0x14 = game frame (logic tick) counter, +1 per tick (0x50a0f0),
//         the same on every machine of a multiplayer game.
// Everything runs on the game thread (executor, move step, GetSpeed, frame hook).
// Multiplayer-safe by design: all decisions depend only on game frames, unit data and command order (no
// wall clock, no own randomness, ordered containers where iteration matters). Wall-clock time is only
// used for log lines. It is still switched off in multiplayer until all players' settings are checked.
namespace
{
  const uintptr_t kExecVa = 0x004c3900;
  const uint8_t kExecOrig[] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xB0, 0x83, 0x5C, 0x00 };
  const uintptr_t kCallSiteVa = 0x004c47ff;
  const uint8_t kCallSiteOrig[] = { 0x8B, 0x11, 0xFF, 0x52, 0x20 }; // mov edx,[ecx]; call [edx+0x20]
  const uintptr_t kGetSpeedVa = 0x00564d30;
  const uint8_t kGetSpeedOrig[] = { 0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x70 };
  const uintptr_t kMoveStepVa = 0x00562f00;
  const uint8_t kMoveStepOrig[] = { 0x6A, 0xFF, 0x68, 0x76, 0xC6, 0x5C, 0x00 };

  const float kTile = 32.0f;
  const float kSpeedFloor = 0.5f;     // never slower than half the unit's own speed
  // in game frames (logic ticks; about 15-20 per second, the measured rate is in the log)
  const int kStuckFrames = 100;       // no progress toward the destination for this long -> not waited for
  const int kGoneFrames = 150;        // no move step for this long -> left the group (gave up, destroyed)
  const int kGroupMaxFrames = 5000;   // a group order is followed for at most a few minutes
  const int kUpdateEvery = 8;         // recompute the speed caps every 8 frames
  const int kSpeedSampleFrames = 15;

  typedef void(__cdecl* Exec_t)(uint8_t* packet);
  typedef float(__fastcall* GetSpeed_t)(void* self, void* edx);
  typedef int(__fastcall* MoveStep_t)(void* self, void* edx, int arg);
  typedef void(__thiscall* Receive_t)(void* self, uint8_t* packet);
  Exec_t realExec = reinterpret_cast<Exec_t>(kExecVa);
  GetSpeed_t realGetSpeed = reinterpret_cast<GetSpeed_t>(kGetSpeedVa);
  MoveStep_t realMoveStep = reinterpret_cast<MoveStep_t>(kMoveStepVa);

  bool gInstalled = false;
  bool gOn = false;        // formation + arrive together active (launcher setting, Ctrl+Shift+F)
  bool gMeasure = false;   // debug log on: measure group orders also when gOn is off
  bool gKeyWasDown = false;
  bool gWasInMission = false;

  // formation targets for the order being executed right now (filled in myExec, read in orderThunk)
  bool gPlanActive = false;
  std::unordered_map<uintptr_t, std::pair<float, float>> gPlan;

  struct Member
  {
    uintptr_t unit = 0;
    float x = 0, z = 0, dx = 0, dz = 0; // last seen position and destination (from the move step)
    float bestD = 1e30f;                // closest distance to the destination so far
    float vmax = 0;                     // own speed without our cap
    int lastStep = 0, lastProgress = 0, vmaxFrame = 0; // game frames
    DWORD arriveWall = 0;               // wall clock, only for the log
    bool arrived = false, dropped = false;
  };
  struct Group
  {
    int id = 0;
    int startFrame = 0;
    DWORD startWall = 0; // only for the log
    bool adjust = false; // formation/speed were applied (feature on when ordered)
    std::vector<Member> members;
  };
  std::map<int, Group> gGroups;
  std::unordered_map<uintptr_t, std::pair<int, size_t>> gMemberOf; // unit -> group id, member index
  std::unordered_map<uintptr_t, float> gCap;                       // unit -> speed cap
  int gNextId = 1;
  int gLastUpdateFrame = -1;

  int gameFrame()
  {
    uintptr_t net = *reinterpret_cast<uintptr_t volatile*>(0x007cb9d4);
    return net ? *reinterpret_cast<int volatile*>(net + 0x14) : 0;
  }

  bool inMission() { return *reinterpret_cast<volatile uint8_t*>(0x00817c0c) != 0; }

  void* setup() { return *reinterpret_cast<void* volatile*>(0x007d75d4); }
  void* world() { return *reinterpret_cast<void* volatile*>(0x00817c00); }

  bool singlePlayer()
  {
    void* s = setup();
    return s && reinterpret_cast<int(__thiscall*)(void*)>(0x005344e0)(s) != 3;
  }

  int playerSide()
  {
    void* s = setup();
    return s ? reinterpret_cast<int(__thiscall*)(void*)>(0x005356a0)(s) : -1;
  }

  uintptr_t findObject(int* id)
  {
    void* w = world();
    return w ? reinterpret_cast<uintptr_t>(reinterpret_cast<void*(__thiscall*)(void*, int*)>(0x0059b350)(w, id)) : 0;
  }

  template <class T> T at(uintptr_t p, uintptr_t off) { return *reinterpret_cast<T*>(p + off); }

  const char* modeText(bool adjust) { return adjust ? "Gruppenbewegung an" : "Gruppenbewegung aus"; }

  void dropMember(Member& m, const char* why)
  {
    if (m.arrived || m.dropped)
      return;
    m.dropped = true;
    gCap.erase(m.unit);
    gMemberOf.erase(m.unit);
    (void)why;
  }

  void finishGroup(Group& g, const char* why)
  {
    DWORD now = GetTickCount(); // log only
    std::vector<DWORD> times;
    int dropped = 0;
    for (Member& m : g.members)
    {
      if (m.arrived)
        times.push_back(m.arriveWall - g.startWall);
      else
      {
        if (!m.dropped) dropMember(m, why);
        dropped++;
      }
    }
    std::sort(times.begin(), times.end());
    int frames = gameFrame() - g.startFrame;
    if (now - g.startWall >= 3000 && frames > 0) // for checking the frame-based limits
      Log("Gruppe #%d: Spieltakt %.1f Ticks/s\n", g.id, frames * 1000.0 / (now - g.startWall));
    if (times.empty())
      Log("Gruppe #%d fertig (%s): keine Einheit angekommen, %d ausgeschieden, nach %.1fs [%s]\n", g.id, why, dropped,
          (now - g.startWall) / 1000.0, modeText(g.adjust));
    else
    {
      size_t i90 = (times.size() * 9 + 9) / 10; // 90 % of the arrived units
      i90 = i90 ? i90 - 1 : 0;
      Log("Gruppe #%d fertig (%s): %d/%d angekommen, %d ausgeschieden | erste %.1fs, 90%% %.1fs, letzte %.1fs, "
          "Streuung %.1fs [%s]\n",
          g.id, why, (int)times.size(), (int)g.members.size(), dropped, times.front() / 1000.0, times[i90] / 1000.0,
          times.back() / 1000.0, (times[i90] - times.front()) / 1000.0, modeText(g.adjust));
    }
  }

  void update(int now)
  {
    if (now == gLastUpdateFrame || now % kUpdateEvery != 0)
      return;
    gLastUpdateFrame = now;
    for (auto it = gGroups.begin(); it != gGroups.end();)
    {
      Group& g = it->second;
      int active = 0;
      for (Member& m : g.members)
      {
        if (m.arrived || m.dropped)
          continue;
        if (now - m.lastStep > kGoneFrames)
          dropMember(m, "keine Bewegung mehr");
        else
          active++;
      }
      if (!active || now - g.startFrame > kGroupMaxFrames)
      {
        finishGroup(g, active ? "Zeitlimit" : "alle angekommen oder ausgeschieden");
        it = gGroups.erase(it);
        continue;
      }
      if (g.adjust && gOn)
      {
        // time the slowest-to-arrive unit still needs (in move steps), ignoring stuck units
        float T = 0;
        for (Member& m : g.members)
        {
          if (m.arrived || m.dropped || m.vmax <= 0 || now - m.lastProgress > kStuckFrames)
            continue;
          float d = std::hypot(m.dx - m.x, m.dz - m.z);
          T = std::max(T, d / m.vmax);
        }
        for (Member& m : g.members)
        {
          if (m.arrived || m.dropped)
            continue;
          if (T <= 0 || m.vmax <= 0 || now - m.lastProgress > kStuckFrames)
          {
            gCap.erase(m.unit);
            continue;
          }
          float d = std::hypot(m.dx - m.x, m.dz - m.z);
          gCap[m.unit] = std::clamp(d / T, kSpeedFloor * m.vmax, m.vmax);
        }
      }
      ++it;
    }
  }

  void resetAll(const char* why)
  {
    for (auto& kv : gGroups)
      finishGroup(kv.second, why);
    gGroups.clear();
    gMemberOf.clear();
    gCap.clear();
  }

  // Formation targets ("magic box"): keep each unit's offset from the group centre when the target lies
  // outside the area the group stands on; pull very spread groups together; clamp to the map.
  int planFormation(std::vector<uintptr_t>& units, float tx, float tz, float& scaleOut)
  {
    size_t n = units.size();
    float cx = 0, cz = 0, minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
    for (uintptr_t u : units)
    {
      float x = at<float>(u, 0x14), z = at<float>(u, 0x1c);
      cx += x, cz += z;
      minX = std::min(minX, x), maxX = std::max(maxX, x), minZ = std::min(minZ, z), maxZ = std::max(maxZ, z);
    }
    cx /= n, cz /= n;
    scaleOut = 0;
    if (tx >= minX - kTile && tx <= maxX + kTile && tz >= minZ - kTile && tz <= maxZ + kTile)
      return 0; // short move inside the group: everyone to the point, like the original
    float r = 0;
    for (uintptr_t u : units)
      r = std::max(r, std::hypot(at<float>(u, 0x14) - cx, at<float>(u, 0x1c) - cz));
    float rMax = kTile * std::max(2.0f, 1.25f * std::sqrt(float(n)));
    float scale = r > rMax ? rMax / r : 1.0f;
    void* w = world();
    float mapW = w ? at<int>(reinterpret_cast<uintptr_t>(w), 0xa270) * kTile : 1e9f;
    float mapH = w ? at<int>(reinterpret_cast<uintptr_t>(w), 0xa274) * kTile : 1e9f;
    for (uintptr_t u : units)
    {
      float x = tx + (at<float>(u, 0x14) - cx) * scale, z = tz + (at<float>(u, 0x1c) - cz) * scale;
      x = std::clamp(x, kTile / 2, std::max(kTile / 2, mapW - kTile / 2));
      z = std::clamp(z, kTile / 2, std::max(kTile / 2, mapH - kTile / 2));
      gPlan[u] = { x, z };
    }
    scaleOut = scale;
    return int(n);
  }

  void __cdecl myExec(uint8_t* p)
  {
    if (!p || !inMission())
    {
      realExec(p);
      return;
    }
    int type = at<int>(reinterpret_cast<uintptr_t>(p), 8);
    int side = at<int>(reinterpret_cast<uintptr_t>(p), 0xc);
    int n = std::clamp(at<int>(reinterpret_cast<uintptr_t>(p), 0x30), 0, 64);
    int* ids = reinterpret_cast<int*>(p + 0x34);

    // a new order ends the old group order for these units
    if (n > 0 && !gMemberOf.empty())
      for (int i = 0; i < n; i++)
      {
        uintptr_t u = findObject(&ids[i]);
        auto it = u ? gMemberOf.find(u) : gMemberOf.end();
        if (it != gMemberOf.end())
        {
          auto g = gGroups.find(it->second.first);
          if (g != gGroups.end() && it->second.second < g->second.members.size())
            dropMember(g->second.members[it->second.second], "neuer Befehl");
          else
            gMemberOf.erase(it);
        }
      }

    if (type != 0 || n < 2 || (!gOn && !gMeasure) || !singlePlayer() || side != playerSide())
    {
      realExec(p);
      return;
    }

    std::vector<uintptr_t> units;
    for (int i = 0; i < n; i++)
    {
      uintptr_t u = findObject(&ids[i]);
      if (!u) continue;
      uintptr_t t = at<uintptr_t>(u, 0x70);
      if (!t || at<int>(t, 0) != 1 || at<uint8_t>(t, 0x3bd) || at<int>(u, 0xc) != side)
        continue; // not a unit, aircraft, or not the player's
      units.push_back(u);
    }
    if (units.size() < 2)
    {
      realExec(p);
      return;
    }

    float tx = at<float>(reinterpret_cast<uintptr_t>(p), 0x14), tz = at<float>(reinterpret_cast<uintptr_t>(p), 0x1c);
    float scale = 0;
    int formed = gOn ? planFormation(units, tx, tz, scale) : 0;
    gPlanActive = formed > 0;
    realExec(p);
    gPlanActive = false;
    gPlan.clear();

    int now = gameFrame();
    Group& g = gGroups[gNextId];
    g.id = gNextId++;
    g.startFrame = now;
    g.startWall = GetTickCount();
    g.adjust = gOn;
    for (uintptr_t u : units)
    {
      auto old = gMemberOf.find(u);
      if (old != gMemberOf.end())
        gMemberOf.erase(old);
      Member m;
      m.unit = u;
      m.x = at<float>(u, 0x14), m.z = at<float>(u, 0x1c);
      m.dx = at<float>(u, 0xfc), m.dz = at<float>(u, 0x104);
      m.vmax = realGetSpeed(reinterpret_cast<void*>(u), nullptr);
      m.lastStep = m.lastProgress = m.vmaxFrame = now;
      gMemberOf[u] = { g.id, g.members.size() };
      g.members.push_back(m);
    }
    Log("Gruppe #%d: %d Einheiten, Ziel %d,%d, Spieltick %d, %s%s\n", g.id, (int)units.size(), int(tx / kTile), int(tz / kTile), now,
        modeText(g.adjust),
        !g.adjust ? "" : formed ? (scale < 0.999f ? ", Formation (zusammengezogen)" : ", Formation") : ", kurzer Weg: ohne Formation");
  }

  // replaces "mov edx,[ecx]; call [edx+0x20]" at 0x4c47ff: ecx = unit, [esp+4] = packet
  void __fastcall orderThunk(void* unit, void* /*edx*/, uint8_t* packet)
  {
    Receive_t receive = *reinterpret_cast<Receive_t*>(*reinterpret_cast<uintptr_t*>(unit) + 0x20);
    if (gPlanActive)
    {
      auto it = gPlan.find(reinterpret_cast<uintptr_t>(unit));
      if (it != gPlan.end())
      {
        alignas(8) uint8_t copy[0x138];
        memcpy(copy, packet, sizeof(copy));
        *reinterpret_cast<float*>(copy + 0x14) = it->second.first;
        *reinterpret_cast<float*>(copy + 0x1c) = it->second.second;
        receive(unit, copy);
        return;
      }
    }
    receive(unit, packet);
  }

  float __fastcall myGetSpeed(void* self, void* edx)
  {
    float s = realGetSpeed(self, edx);
    if (gOn && !gCap.empty())
    {
      auto it = gCap.find(reinterpret_cast<uintptr_t>(self));
      if (it != gCap.end() && it->second < s)
        return it->second;
    }
    return s;
  }

  int __fastcall myMoveStep(void* self, void* edx, int arg)
  {
    uintptr_t unit = reinterpret_cast<uintptr_t>(self);
    auto it = gMemberOf.find(unit);
    if (it == gMemberOf.end())
    {
      if (!gGroups.empty()) update(gameFrame());
      return realMoveStep(self, edx, arg);
    }
    // read before the step (the step could in theory remove the unit)
    float x = at<float>(unit, 0x14), z = at<float>(unit, 0x1c), dx = at<float>(unit, 0xfc), dz = at<float>(unit, 0x104);
    int now = gameFrame();
    update(now); // first, so that a group finished here does not leave m dangling
    it = gMemberOf.find(unit);
    if (it == gMemberOf.end())
      return realMoveStep(self, edx, arg);
    Member* m = nullptr;
    auto g = gGroups.find(it->second.first);
    if (g != gGroups.end() && it->second.second < g->second.members.size())
      m = &g->second.members[it->second.second];
    if (m && now - m->vmaxFrame >= kSpeedSampleFrames)
    {
      m->vmax = realGetSpeed(self, nullptr);
      m->vmaxFrame = now;
    }
    int ret = realMoveStep(self, edx, arg);
    if (m)
    {
      m->x = x, m->z = z, m->dx = dx, m->dz = dz, m->lastStep = now;
      float d = std::hypot(dx - x, dz - z);
      if (d < m->bestD - kTile / 2)
      {
        m->bestD = d;
        m->lastProgress = now;
      }
      if (ret == 0)
      {
        m->arrived = true;
        m->arriveWall = GetTickCount(); // log only
        gCap.erase(unit);
        gMemberOf.erase(unit);
      }
    }
    return ret;
  }

  bool patchCallSite()
  {
    uint8_t* site = reinterpret_cast<uint8_t*>(kCallSiteVa);
    DWORD old = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
      return false;
    site[0] = 0xE8;
    int32_t rel = int32_t(reinterpret_cast<uintptr_t>(&orderThunk) - (kCallSiteVa + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
  }

  bool keyDown()
  {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
           (GetAsyncKeyState('F') & 0x8000);
  }
}

void initGroupMove(bool enabled, bool debugLog)
{
  gOn = enabled;
  gMeasure = debugLog;
  if (!enabled && !debugLog)
  {
    Log("GroupMove: off\n");
    return;
  }
  if (memcmp(reinterpret_cast<const void*>(kExecVa), kExecOrig, sizeof(kExecOrig)) != 0 ||
      memcmp(reinterpret_cast<const void*>(kCallSiteVa), kCallSiteOrig, sizeof(kCallSiteOrig)) != 0 ||
      memcmp(reinterpret_cast<const void*>(kGetSpeedVa), kGetSpeedOrig, sizeof(kGetSpeedOrig)) != 0 ||
      (memcmp(reinterpret_cast<const void*>(kMoveStepVa), kMoveStepOrig, sizeof(kMoveStepOrig)) != 0 &&
       *reinterpret_cast<const uint8_t*>(kMoveStepVa) != 0xE9)) // E9 = already detoured by the jam reporter
  {
    Log("GroupMove: unexpected code in Game.exe, not hooking\n");
    gOn = false;
    return;
  }
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)realExec, reinterpret_cast<PVOID>(myExec));
  DetourAttach(&(PVOID&)realGetSpeed, reinterpret_cast<PVOID>(myGetSpeed));
  DetourAttach(&(PVOID&)realMoveStep, reinterpret_cast<PVOID>(myMoveStep));
  LONG e = DetourTransactionCommit();
  if (e != NO_ERROR || !patchCallSite())
  {
    Log("GroupMove: hook FAILED (%ld)\n", e);
    gOn = false;
    return;
  }
  gInstalled = true;
  gPlan.reserve(64);
  Log("GroupMove: %s (Ctrl+Shift+F schaltet im Spiel um; Messung %s)\n", gOn ? "an" : "aus", gMeasure ? "an" : "aus");
}

void groupMoveUpdateFrame()
{
  if (!gInstalled)
    return;
  bool mission = inMission();
  if (!mission && gWasInMission)
    resetAll("Mission beendet");
  gWasInMission = mission;

  bool down = keyDown();
  if (down && !gKeyWasDown && mission)
  {
    gOn = !gOn;
    if (!gOn)
      gCap.clear();
    bool de = gameMessageGerman();
    if (!singlePlayer())
      showGameMessage(de ? "Gruppenbewegung: im Mehrspieler nicht verfuegbar" : "Group movement: not available in multiplayer");
    else if (gOn)
      showGameMessage(de ? "Gruppenbewegung: AN" : "Group movement: ON");
    else
      showGameMessage(de ? "Gruppenbewegung: AUS" : "Group movement: OFF");
  }
  gKeyWasDown = down;
}
