#pragma once
// Platform-independent part of the HD texture workshop (see TextureWorkshop.hpp): reading the game
// archives, sorting the textures, the recipes before/after Real-ESRGAN, and writing/moving the result
// archive. Upscaling itself is done by a runner function supplied by the caller (Windows: the
// launcher starts realesrgan-ncnn-vulkan.exe; tests: the Linux build of the same program).
// stb: define STB_IMAGE_IMPLEMENTATION and STB_IMAGE_WRITE_IMPLEMENTATION in exactly one translation unit
// before including this file (TextureWorkshop.cpp does).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "HdImage.hpp"
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#include "third_party/miniz.h" // zlib streams for the archive entries (better ratio than stb's)

namespace hdCore
{
  namespace fs = std::filesystem;
  using hdImage::Img;

  inline const char kHdMarker[] = "EmperorSpice\\HDTextures.txt";
  inline const char kModMarker[] = "EmperorSpice\\ModPack.txt";
  inline const char kRecipeVersion[] = "HD textures v5 (icons 2x, units/buildings 2x + remaster, terrain 4x; zlib, parts; fonts and UI markers kept)";

  // The game hangs at the main menu with one big uncompressed archive (340 MB, tested 15.11.). So the
  // result is written like the game's own archives: zlib-compressed entries, split into parts that are
  // no larger than this (the game's own 3DDATA0001.RFD is 68 MB).
#ifndef HD_MAX_PART_BYTES
#define HD_MAX_PART_BYTES (40u * 1024 * 1024)
#endif
  inline const uint32_t kMaxPartBytes = HD_MAX_PART_BYTES; // overridable for tests

  inline std::string lowerA(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
    return s;
  }

  inline fs::path archivePath(const fs::path& dir, const std::string& prefix, int n, const char* ext)
  {
    char name[64];
    snprintf(name, sizeof(name), "%s%04d%s", prefix.c_str(), n, ext);
    return dir / name;
  }

  inline bool readFile(const fs::path& p, std::vector<char>& out)
  {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
  }

  // ---------------------------------------------------------------- archives
  struct Entry
  {
    std::string name;
    uint32_t ts = 0, flags = 0, stored = 0, raw = 0, off = 0;
    fs::path rfd;
  };

  inline std::vector<Entry> readIndex(const fs::path& rfh, const fs::path& rfd)
  {
    std::vector<Entry> out;
    std::vector<char> d;
    if (!readFile(rfh, d)) return out;
    size_t pos = 0;
    while (pos + 24 <= d.size())
    {
      uint32_t v[6];
      memcpy(v, &d[pos], 24);
      pos += 24;
      if (v[0] == 0 || pos + v[0] > d.size()) break;
      Entry e;
      e.name.assign(&d[pos], strnlen(&d[pos], v[0]));
      e.ts = v[1]; e.flags = v[2]; e.stored = v[3]; e.raw = v[4]; e.off = v[5]; e.rfd = rfd;
      out.push_back(e);
      pos += v[0];
    }
    return out;
  }

  inline bool hasEntry(const std::vector<Entry>& idx, const char* name)
  {
    for (const Entry& e : idx) if (lowerA(e.name) == lowerA(name)) return true;
    return false;
  }

  inline bool extract(const Entry& e, std::vector<char>& out)
  {
    std::ifstream f(e.rfd, std::ios::binary);
    if (!f) return false;
    std::vector<char> blob(e.stored);
    f.seekg(e.off);
    if (!f.read(blob.data(), blob.size())) return false;
    if (!(e.flags & 2)) { out.swap(blob); return true; }
    if (blob.size() < 4) return false;
    out.assign(e.raw, 0);
    int n = stbi_zlib_decode_buffer(out.data(), int(out.size()), blob.data() + 4, int(blob.size() - 4));
    return n == int(e.raw);
  }

  // Archives <prefix>0001.. in load order (stops at the first gap, like the game).
  inline std::vector<int> archiveNumbers(const fs::path& data, const std::string& prefix)
  {
    std::vector<int> out;
    std::error_code ec;
    for (int n = 1; n < 100 && fs::exists(archivePath(data, prefix, n, ".RFH"), ec); n++) out.push_back(n);
    return out;
  }

  // Numbers of our HD archives in DATA (in order), empty if switched off.
  inline std::vector<int> activeHdArchives(const fs::path& data)
  {
    std::vector<int> out;
    for (int n : archiveNumbers(data, "3DDATA"))
      if (hasEntry(readIndex(archivePath(data, "3DDATA", n, ".RFH"), {}), kHdMarker)) out.push_back(n);
    return out;
  }
  inline int activeHdArchive(const fs::path& data) // first one, or 0 (tests)
  {
    std::vector<int> a = activeHdArchives(data);
    return a.empty() ? 0 : a.front();
  }

  // Renumber <prefix> archives so that no gap is left (keeps order).
  inline std::string compact(const fs::path& data, const std::string& prefix)
  {
    std::error_code ec;
    std::vector<int> present;
    for (int n = 1; n < 100; n++) // our parts may leave several gaps
      if (fs::exists(archivePath(data, prefix, n, ".RFH"), ec)) present.push_back(n);
    int target = 1;
    for (int n : present)
    {
      if (n != target)
        for (const char* ext : { ".RFD", ".RFH" }) // index last
        {
          fs::path from = archivePath(data, prefix, n, ext);
          if (fs::exists(from, ec)) fs::rename(from, archivePath(data, prefix, target, ext), ec);
          if (ec) return "rename " + from.filename().string() + " failed";
        }
      target++;
    }
    return "";
  }

  inline fs::path dataDir(const fs::path& gameDir) { return gameDir / "DATA"; }
  inline fs::path cacheDir(const fs::path& gameDir) { return gameDir / "hdtextures"; }
  // Generated parts while switched off: <game>\hdtextures\3DDATA_HD_01.RFH/.RFD, _02 ...
  inline fs::path cachePart(const fs::path& gameDir, int part, const char* ext)
  {
    char name[32];
    snprintf(name, sizeof(name), "3DDATA_HD_%02d%s", part, ext);
    return cacheDir(gameDir) / name;
  }
  inline fs::path versionFile(const fs::path& gameDir) { return cacheDir(gameDir) / "version.txt"; }

  inline int cachedParts(const fs::path& gameDir)
  {
    std::error_code ec;
    int n = 0;
    while (n < 99 && fs::exists(cachePart(gameDir, n + 1, ".RFH"), ec)) n++;
    return n;
  }

  // Removes results of older workshop versions (one big archive - hangs the game) and old parts.
  inline void removeCache(const fs::path& gameDir)
  {
    std::error_code ec;
    for (const char* f : { "3DDATA_HD.RFH", "3DDATA_HD.RFD", "3DDATA_HD.RFD.tmp" }) fs::remove(cacheDir(gameDir) / f, ec);
    for (int n = 1; n < 100; n++)
      for (const char* ext : { ".RFH", ".RFD", ".RFD.tmp" }) fs::remove(cachePart(gameDir, n, ext), ec);
    fs::remove(versionFile(gameDir), ec);
  }

  inline bool currentVersion(const fs::path& gameDir)
  {
    std::vector<char> v;
    return readFile(versionFile(gameDir), v) && std::string(v.begin(), v.end()) == kRecipeVersion;
  }

  // Generated with the current recipe (switched on or off).
  inline bool generated(const fs::path& gameDir)
  {
    return currentVersion(gameDir) && (cachedParts(gameDir) > 0 || !activeHdArchives(dataDir(gameDir)).empty());
  }

  inline std::string apply(const fs::path& gameDir, bool enabled)
  {
    std::error_code ec;
    const fs::path data = dataDir(gameDir);
    const std::vector<int> active = activeHdArchives(data);
    if (enabled)
    {
      const int parts = cachedParts(gameDir);
      if (!active.empty() || parts == 0 || !currentVersion(gameDir)) return "";
      std::vector<int> nums = archiveNumbers(data, "3DDATA");
      const int first = nums.empty() ? 1 : nums.back() + 1;
      if (first + parts > 100) return "no free 3DDATA archive number";
      for (int i = 0; i < parts; i++)
      {
        fs::rename(cachePart(gameDir, i + 1, ".RFD"), archivePath(data, "3DDATA", first + i, ".RFD"), ec);   // data first,
        if (!ec) fs::rename(cachePart(gameDir, i + 1, ".RFH"), archivePath(data, "3DDATA", first + i, ".RFH"), ec); // index last
        if (ec) return "move HD archive into DATA: " + ec.message();
      }
      return "";
    }
    if (active.empty()) return "";
    fs::create_directories(cacheDir(gameDir), ec);
    for (int n = 1; n < 100; n++)
      for (const char* ext : { ".RFH", ".RFD" }) fs::remove(cachePart(gameDir, n, ext), ec);
    for (size_t i = 0; i < active.size(); i++)
    {
      const int part = int(i) + 1;
      fs::rename(archivePath(data, "3DDATA", active[i], ".RFH"), cachePart(gameDir, part, ".RFH"), ec); // index first: game ignores it then
      if (!ec) fs::rename(archivePath(data, "3DDATA", active[i], ".RFD"), cachePart(gameDir, part, ".RFD"), ec);
      if (ec) return "move HD archive out of DATA: " + ec.message();
    }
    return compact(data, "3DDATA");
  }

  // ---------------------------------------------------------------- sorting
  enum Kind { KindSkip, KindIcon, KindModel, KindTerrain };

  inline bool isEffect(const std::string& lowerBase)
  {
    std::string s = lowerBase;
    while (!s.empty() && s[0] == '=') s.erase(0, 1);
    if (!s.empty() && (s[0] == '!' || s[0] == '@' || s[0] == '%')) return true;
    for (const char* k : { "flash", "spotlight", "halo", "beam", "glow", "muzzle" })
      if (s.find(k) != std::string::npos) return true;
    return false;
  }

  // Font textures: the game cuts the letters out by pixel position, so a bigger texture breaks the text
  // (an upscaled Standard.tga hung the game while building the main menu, 15.11.).
  inline bool isFont(const std::string& lowerBase)
  {
    for (const char* f : { "font", "small", "standard" })
      if (lowerBase.rfind(f, 0) == 0) return true;
    return false;
  }

  // Flat UI markers drawn on the battlefield (selection and range rings, waypoint chevrons, power bar,
  // language flags, icon sheets): the remaster's relief and dust don't suit them, they stay original.
  inline bool isUiMarker(const std::string& lowerBase)
  {
    for (const char* k : { "ring", "chevron", "powerbar", "icons", "flag_gr", "flag_ko", "flag_uk", "flag_usa" })
      if (lowerBase.find(k) != std::string::npos) return true;
    return false;
  }

  // Texture names used by the map terrain meshes (test.xbf: texture name blob after the header).
  inline std::set<std::string> terrainTextureNames(const fs::path& data)
  {
    std::set<std::string> names;
    for (int n : archiveNumbers(data, "MAPS"))
      for (const Entry& e : readIndex(archivePath(data, "MAPS", n, ".RFH"), archivePath(data, "MAPS", n, ".RFD")))
      {
        std::string ln = lowerA(e.name);
        if (ln.size() < 9 || ln.compare(ln.size() - 9, 9, "\\test.xbf") != 0) continue;
        std::vector<char> x;
        if (!extract(e, x) || x.size() < 12) continue;
        int32_t version, fxSize, blobLen;
        memcpy(&version, &x[0], 4); memcpy(&fxSize, &x[4], 4);
        if (version != 1 || fxSize < 0 || size_t(fxSize) + 12 > x.size()) continue;
        memcpy(&blobLen, &x[8 + size_t(fxSize)], 4);
        size_t p = 12 + size_t(fxSize);
        if (blobLen < 0 || p + size_t(blobLen) > x.size()) continue;
        std::string cur;
        for (size_t i = p; i < p + size_t(blobLen); i++)
        {
          char c = x[i];
          if (c == '\0') { if (!cur.empty()) names.insert(lowerA(cur)); cur.clear(); }
          else if (c != '\x02') cur += c;
        }
      }
    return names;
  }

  struct Item
  {
    Entry entry;
    std::string base; // lower case file name, e.g. "at_trike.tga"
    Kind kind = KindSkip;
  };

  // Newest version of every Textures\*.tga in the 3DDATA archives (ours excluded), sorted into recipes.
  inline std::vector<Item> collect(const fs::path& data)
  {
    std::map<std::string, Entry> newest;
    for (int n : archiveNumbers(data, "3DDATA"))
    {
      std::vector<Entry> idx = readIndex(archivePath(data, "3DDATA", n, ".RFH"), archivePath(data, "3DDATA", n, ".RFD"));
      if (hasEntry(idx, kHdMarker) || hasEntry(idx, kModMarker)) continue;
      for (const Entry& e : idx)
      {
        std::string ln = lowerA(e.name);
        if (ln.rfind("textures\\", 0) != 0 || ln.size() < 5 || ln.compare(ln.size() - 4, 4, ".tga") != 0) continue;
        auto it = newest.find(ln);
        if (it == newest.end() || e.ts >= it->second.ts) newest[ln] = e;
      }
    }
    std::set<std::string> bases;
    for (auto& kv : newest) bases.insert(kv.first.substr(9));
    const std::set<std::string> terrain = terrainTextureNames(data);

    std::vector<Item> items;
    for (auto& kv : newest)
    {
      Item it;
      it.entry = kv.second;
      it.base = kv.first.substr(9);
      if (it.base.rfind("grey_", 0) == 0 || bases.count("grey_" + it.base)) it.kind = KindIcon;
      else if (isEffect(it.base) || isFont(it.base) || isUiMarker(it.base)) it.kind = KindSkip;
      else if (terrain.count(it.base)) it.kind = KindTerrain;
      else it.kind = KindModel;
      if (it.kind != KindSkip) items.push_back(it);
    }
    return items;
  }

  // ---------------------------------------------------------------- recipes
  struct Job
  {
    Item item;
    Img original;           // as loaded (3 or 4 channels)
    bool hasAlpha = false;
    std::vector<uint8_t> key; // magenta colour key of the original
    int pad = 0;            // terrain: wrap padding before upscaling
    bool ok = false;
  };

  // Loads the texture and returns the RGB image to send to Real-ESRGAN (false = keep original).
  inline bool prepare(Job& job, Img& toUpscale)
  {
    std::vector<char> raw;
    if (!extract(job.item.entry, raw) || !hdImage::readTga(raw, job.original, job.hasAlpha)) return false;
    const int w = job.original.w, h = job.original.h;
    if (job.item.kind == KindIcon)
    {
      // Build icons are 64x64; a few are smaller (worm, Death Hand: 60x60, Guild Maker: 54x50).
      if (w > 64 || h > 64 || std::min(w, h) < 32) return false;
      toUpscale = hdImage::descan(hdImage::rgbOf(job.original));
      return true;
    }
    if (std::min(w, h) < 16 || std::max(w, h) > 256) return false;
    Img rgb = hdImage::rgbOf(job.original);
    job.key = hdImage::magentaKey(rgb);
    if (std::count(job.key.begin(), job.key.end(), 1) == int(job.key.size())) return false; // fully transparent
    hdImage::fillMasked(rgb, job.key);
    if (job.item.kind == KindTerrain)
    {
      job.pad = std::max(8, w / 4);
      toUpscale = hdImage::padWrap(rgb, job.pad);
    }
    else
      toUpscale = rgb;
    return true;
  }

  // Takes Real-ESRGAN's 4x result and returns the final texture.
  inline Img finish(const Job& job, const Img& up4)
  {
    const int w = job.original.w, h = job.original.h;
    Img rgb;
    int fw, fh;
    if (job.item.kind == KindIcon)
    {
      fw = 2 * w; fh = 2 * h; // 64 -> 128
      rgb = hdImage::resizeLanczos(up4, fw, fh);
      hdImage::clampTo8bit(rgb);
    }
    else if (job.item.kind == KindTerrain)
    {
      rgb = hdImage::crop(up4, 4 * job.pad, 4 * job.pad, 4 * w, 4 * h);
      rgb = hdImage::dampSandLines(rgb);
      fw = 4 * w; fh = 4 * h;
    }
    else
    {
      Img r = hdImage::remaster(up4);
      // keep transparent texels out of the downscale filter
      std::vector<uint8_t> key4 = hdImage::scaleMask(job.key, w, h, r.w, r.h);
      hdImage::fillMasked(r, key4);
      fw = 2 * w; fh = 2 * h;
      rgb = hdImage::resizeLanczos(r, fw, fh);
      hdImage::clampTo8bit(rgb);
      hdImage::restoreHouseColour(rgb, job.original);
    }
    std::vector<uint8_t> keyF = job.key.empty() ? std::vector<uint8_t>() : hdImage::scaleMask(job.key, w, h, fw, fh);
    for (size_t i = 0; i < keyF.size(); i++)
      if (keyF[i]) { rgb.d[i * 3] = 255; rgb.d[i * 3 + 1] = 0; rgb.d[i * 3 + 2] = 255; }
    if (!job.hasAlpha) return rgb;
    Img alpha(w, h, 1);
    for (size_t i = 0; i < size_t(w) * h; i++) alpha.d[i] = job.original.d[i * 4 + 3];
    alpha = hdImage::resizeLanczos(alpha, fw, fh);
    Img out(fw, fh, 4);
    for (size_t i = 0; i < size_t(fw) * fh; i++)
    {
      for (int k = 0; k < 3; k++) out.d[i * 4 + k] = rgb.d[i * 3 + k];
      out.d[i * 4 + 3] = std::min(255.0f, std::max(0.0f, std::round(alpha.d[i])));
    }
    return out;
  }

  // ---------------------------------------------------------------- generate
  // Entry format of the game's own archives (flag 2): u32 raw size, then a zlib stream.
  inline std::vector<char> compressEntry(const std::vector<char>& raw)
  {
    mz_ulong zlen = mz_compressBound(mz_ulong(raw.size()));
    std::vector<char> out(4 + size_t(zlen));
    uint32_t n = uint32_t(raw.size());
    memcpy(out.data(), &n, 4);
    if (mz_compress2(reinterpret_cast<unsigned char*>(out.data() + 4), &zlen, reinterpret_cast<const unsigned char*>(raw.data()),
                     mz_ulong(raw.size()), 9) != MZ_OK)
      return {};
    out.resize(4 + size_t(zlen));
    return out;
  }


  struct Progress
  {
    std::function<void(int stage, int done, int total)> report;
    std::function<bool()> cancelled;
  };

  // Upscales every PNG in inDir 4x into outDir (same file names). Calls poll() regularly; poll()
  // returning false means: stop now. Returns "" or an error text.
  using Runner = std::function<std::string(const fs::path& inDir, const fs::path& outDir, const std::function<bool()>& poll)>;

  // PNG via memory, so that paths with any characters work (stb's own fopen is 8-bit only).
  inline bool writePng(const fs::path& p, const Img& im)
  {
    std::vector<uint8_t> px(im.d.size());
    for (size_t i = 0; i < px.size(); i++) px[i] = uint8_t(std::min(255.0f, std::max(0.0f, std::round(im.d[i]))));
    std::vector<char> bytes;
    auto sink = [](void* ctx, void* data, int size) {
      auto* v = static_cast<std::vector<char>*>(ctx);
      v->insert(v->end(), static_cast<char*>(data), static_cast<char*>(data) + size);
    };
    if (!stbi_write_png_to_func(sink, &bytes, im.w, im.h, im.c, px.data(), im.w * im.c)) return false;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), bytes.size());
    return bool(f);
  }

  inline bool readPng(const fs::path& p, Img& im)
  {
    std::vector<char> bytes;
    if (!readFile(p, bytes)) return false;
    int w, h, c;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()), &w, &h, &c, 3);
    if (!px) return false;
    im = Img(w, h, 3);
    for (size_t i = 0; i < im.d.size(); i++) im.d[i] = px[i];
    stbi_image_free(px);
    return true;
  }

  // stage: 0 reading, 1 upscaling, 2 packing
  inline std::string generate(const fs::path& gameDir, const fs::path& workDir, const Runner& run, const Progress& progress,
                              int batchSize = 48, int threads = 4, const std::set<std::string>* only = nullptr)
  {
    std::error_code ec;
    const fs::path data = dataDir(gameDir);
    if (!fs::is_directory(data, ec)) return "DATA folder not found";
    progress.report(0, 0, 0);

    // an old result must not be read as game data, and gets replaced anyway
    std::string err = apply(gameDir, false);
    if (!err.empty()) return err;
    fs::create_directories(cacheDir(gameDir), ec);
    removeCache(gameDir);

    std::vector<Item> items = collect(data);
    if (only)
      items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& i) { return !only->count(i.base); }), items.end());
    if (items.empty()) return "no textures found in the 3DDATA archives";

    // Archive parts, written while the textures come in. Each part carries the marker entry.
    const uint32_t now = uint32_t(time(nullptr));
    const std::string marker = std::string("Emperor Spice: ") + kRecipeVersion +
                               ". Made by the launcher from your own game files - do not share.\r\n";
    std::mutex writeLock;
    int part = 0;
    std::ofstream rfd;
    std::vector<char> rfh;
    uint32_t offset = 0;
    std::string writeError;
    auto put32 = [&](uint32_t x) { rfh.insert(rfh.end(), reinterpret_cast<char*>(&x), reinterpret_cast<char*>(&x) + 4); };
    // stored = what is written to the RFD (for zlib entries: u32 raw size + zlib stream, flag 2)
    auto addRaw = [&](const std::string& name, uint32_t flags, const std::vector<char>& stored, uint32_t rawSize) {
      put32(uint32_t(name.size() + 1)); put32(now); put32(flags); put32(uint32_t(stored.size())); put32(rawSize); put32(offset);
      rfh.insert(rfh.end(), name.begin(), name.end()); rfh.push_back('\0');
      rfd.write(stored.data(), stored.size());
      offset += uint32_t(stored.size());
    };
    auto closePart = [&]() -> bool {
      if (part == 0) return true;
      addRaw(kHdMarker, 0, std::vector<char>(marker.begin(), marker.end()), uint32_t(marker.size()));
      rfd.close();
      if (!rfd) return false;
      std::ofstream f(cachePart(gameDir, part, ".RFH"), std::ios::binary | std::ios::trunc);
      f.write(rfh.data(), rfh.size());
      if (!f) return false;
      f.close();
      fs::rename(cachePart(gameDir, part, ".RFD.tmp"), cachePart(gameDir, part, ".RFD"), ec);
      return !ec;
    };
    auto openPart = [&]() -> bool {
      part++;
      rfh.clear();
      offset = 0;
      rfd.open(cachePart(gameDir, part, ".RFD.tmp"), std::ios::binary | std::ios::trunc);
      return bool(rfd);
    };
    // called under writeLock
    auto addEntry = [&](const std::string& name, const std::vector<char>& stored, uint32_t rawSize) {
      if (part == 0 || offset + stored.size() > kMaxPartBytes)
        if (!closePart() || !openPart()) { if (writeError.empty()) writeError = "cannot write the HD archive"; return; }
      addRaw(name, 2, stored, rawSize);
    };

    const fs::path inDir = workDir / "in", outDir = workDir / "out";
    int done = 0, written = 0;
    const int total = int(items.size());
    auto cleanup = [&] { fs::remove_all(workDir, ec); };
    auto failed = [&](const std::string& msg) { rfd.close(); removeCache(gameDir); cleanup(); return msg; };

    for (size_t b0 = 0; b0 < items.size(); b0 += size_t(batchSize))
    {
      if (progress.cancelled()) return failed("cancelled");
      fs::remove_all(workDir, ec);
      fs::create_directories(inDir, ec);
      fs::create_directories(outDir, ec);
      const size_t b1 = std::min(items.size(), b0 + size_t(batchSize));
      std::vector<Job> jobs(b1 - b0);
      for (size_t i = b0; i < b1; i++)
      {
        Job& job = jobs[i - b0];
        job.item = items[i];
        Img up;
        char name[32];
        snprintf(name, sizeof(name), "%05d.png", int(i));
        job.ok = prepare(job, up) && writePng(inDir / name, up);
      }

      progress.report(1, done, total);
      err = run(inDir, outDir, [&] {
        int n = 0;
        for (auto& e : fs::directory_iterator(outDir, ec)) { (void)e; n++; }
        progress.report(1, std::min(total - 1, done + n), total);
        return !progress.cancelled();
      });
      if (progress.cancelled()) return failed("cancelled");
      if (!err.empty()) return failed(err);

      // finishing in parallel, writing in order of completion
      std::atomic<size_t> next{ 0 };
      std::string threadError;
      auto worker = [&] {
        for (size_t j; (j = next++) < jobs.size();)
        {
          if (progress.cancelled()) return;
          Job& job = jobs[j];
          if (!job.ok) continue;
          char name[32];
          snprintf(name, sizeof(name), "%05d.png", int(b0 + j));
          Img up4;
          if (!readPng(outDir / name, up4))
          {
            std::lock_guard<std::mutex> g(writeLock);
            if (threadError.empty()) threadError = "Real-ESRGAN produced no result for " + job.item.entry.name;
            return;
          }
          std::vector<char> tga = hdImage::writeTga(finish(job, up4));
          std::vector<char> stored = compressEntry(tga); // in this thread, before taking the lock
          std::lock_guard<std::mutex> g(writeLock);
          if (stored.empty()) { if (writeError.empty()) writeError = "compression failed"; return; }
          addEntry(job.item.entry.name, stored, uint32_t(tga.size()));
          written++;
        }
      };
      std::vector<std::thread> pool;
      for (int t = 0; t < std::max(1, threads); t++) pool.emplace_back(worker);
      for (auto& t : pool) t.join();
      if (progress.cancelled()) return failed("cancelled");
      if (!threadError.empty()) return failed(threadError);
      if (!writeError.empty() || (part > 0 && !rfd)) return failed(writeError.empty() ? "cannot write the HD archive" : writeError);
      done = int(b1);
      progress.report(1, done, total);
    }

    progress.report(2, total, total);
    if (!closePart()) return failed("cannot write the HD archive");
    {
      std::ofstream f(versionFile(gameDir), std::ios::binary | std::ios::trunc);
      f << kRecipeVersion;
      if (!f) return failed("cannot write " + versionFile(gameDir).string());
    }
    cleanup();
    if (written == 0) return "no texture could be converted";
    return "";
  }
}
