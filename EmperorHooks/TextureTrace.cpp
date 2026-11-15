#include <windows.h>
#include <cstdint>
#include <cstring>
#include <detours.h>
#include "TextureTrace.hpp"
#include "Log.hpp"

// Game.exe 1.09 (see docs/reverse-engineering.md):
//   0x403490  ResourceLibrary::Load (thiscall: name, uint32* size, 0, 0) -> malloc'd file data or 0.
//             Used by the texture loader at 0x54c2cf ("Failed to find texture %s" when it returns 0).
//   0x417810  texture create from the loaded file data (cdecl: data, info) -> texture object.
// Both are wrapped; each step is written to emperor.txt before it runs, so the last line before a hang
// names the file and the step.
namespace
{
  const uintptr_t kLoadVa = 0x00403490;
  const uint8_t kLoadOrig[] = { 0x81, 0xEC, 0x10, 0x02, 0x00, 0x00 };
  const uintptr_t kCreateVa = 0x00417810;
  const uint8_t kCreateOrig[] = { 0x6A, 0xFF, 0x68, 0x9B, 0x2E, 0x5C, 0x00 };
  const int kMaxLines = 4000;

  typedef void*(__fastcall* Load_t)(void* self, void* edx, const char* name, uint32_t* size, int a, int b);
  typedef void*(__cdecl* Create_t)(void* data, void* info);
  Load_t realLoad = reinterpret_cast<Load_t>(kLoadVa);
  Create_t realCreate = reinterpret_cast<Create_t>(kCreateVa);

  volatile LONG lines = 0;
  bool inMission() { return *reinterpret_cast<volatile uint8_t*>(0x00817c0c) != 0; }

  bool active()
  {
    if (inMission() || lines >= kMaxLines) return false;
    if (InterlockedIncrement(&lines) == kMaxLines) { Log("TexTrace: line limit reached, stopping\n"); return false; }
    return true;
  }

  bool isTexture(const char* name)
  {
    size_t n = name ? strlen(name) : 0;
    return n > 4 && _stricmp(name + n - 4, ".tga") == 0;
  }

  void* __fastcall myLoad(void* self, void* edx, const char* name, uint32_t* size, int a, int b)
  {
    const bool trace = isTexture(name) && active();
    if (trace) Log("TexTrace: load %s\n", name);
    void* r = realLoad(self, edx, name, size, a, b);
    if (trace) Log("TexTrace: loaded %s (%lu bytes)%s\n", name, size ? (unsigned long)*size : 0ul, r ? "" : " - NOT FOUND");
    return r;
  }

  void* __cdecl myCreate(void* data, void* info)
  {
    const bool trace = active();
    if (trace && data && *static_cast<uint8_t*>(data) == 0) // uncompressed TGA header: width/height at 12/14
    {
      const uint8_t* d = static_cast<const uint8_t*>(data);
      Log("TexTrace: create %dx%d %d bit\n", d[12] | d[13] << 8, d[14] | d[15] << 8, d[16]);
    }
    else if (trace)
      Log("TexTrace: create\n");
    void* r = realCreate(data, info);
    if (trace) Log("TexTrace: created -> %p\n", r);
    return r;
  }
}

void initTextureTrace(bool enabled)
{
  if (!enabled) return;
  if (memcmp(reinterpret_cast<const void*>(kLoadVa), kLoadOrig, sizeof(kLoadOrig)) != 0 ||
      memcmp(reinterpret_cast<const void*>(kCreateVa), kCreateOrig, sizeof(kCreateOrig)) != 0)
  {
    Log("TexTrace: unexpected code at %08lX/%08lX, not hooking\n", (unsigned long)kLoadVa, (unsigned long)kCreateVa);
    return;
  }
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)realLoad, reinterpret_cast<PVOID>(myLoad));
  DetourAttach(&(PVOID&)realCreate, reinterpret_cast<PVOID>(myCreate));
  LONG e = DetourTransactionCommit();
  Log("TexTrace: %s (texture loads until the first mission)\n", e == NO_ERROR ? "on" : "hook FAILED");
}
