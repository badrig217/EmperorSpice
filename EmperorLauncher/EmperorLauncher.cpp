#include <Windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include <detours.h>
#include <cstdio>
#include <string>
#include <CommCtrl.h>
#include <map>
#include <algorithm>
#include <cwctype>
#include <optional>
#include <iterator>
#include "../EmperorHooks/Settings.hpp"
#include "../EmperorHooks/Error.hpp"
#include "Installer.hpp"
#include "Installer.hpp"
#include <filesystem>
#include "md5.h"
#include "NetworkTest.hpp"
#include "resource.h"
#include "Lang.hpp"
#include "GraphicsPack.hpp"
#include "FontPack.hpp"
#include "ModPack.hpp"
#include "TextureWorkshopUi.hpp"
#include "LauncherLog.hpp"
#include "LauncherUi.hpp"
#include <vector>
#include "Shortcut.hpp"
#include "IsoReader.hpp"
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

// Modern Windows look for buttons, checkboxes and combo boxes (Common Controls v6). Without this the
// launcher's controls are drawn in the Windows 95 style.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")


HANDLE globalCommsFileMappingHandle = nullptr;
HANDLE globalCommsHandleMutex = nullptr;

const char* globalCommsHandleMutexGuid = "48BC11BD-C4D7-466b-8A31-C6ABBAD47B3E";
const char* globalCommsEventNameGuid = "D6E7FC97-64F9-4d28-B52C-754EDF721C6F";
const char* decryptedEmperorDatData = "UIDATA,3DDATA,MAPS";


void closeGlobalCommsHandle()
{
  if (globalCommsFileMappingHandle)
  {
    CloseHandle(globalCommsFileMappingHandle);
    globalCommsFileMappingHandle = nullptr;
  }

  if (globalCommsHandleMutex)
  {
    CloseHandle(globalCommsHandleMutex);
    globalCommsHandleMutex = nullptr;
  }
}

void createGlobalCommsFileMappingHandle()
{
  closeGlobalCommsHandle();

  SetLastError(0);
  globalCommsHandleMutex = CreateMutexA(0, 0, globalCommsHandleMutexGuid);
  release_assert(globalCommsHandleMutex && GetLastError() == 0);

  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = 12;
  attributes.lpSecurityDescriptor = 0;
  attributes.bInheritHandle = 1;

  SetLastError(0);
  globalCommsFileMappingHandle = CreateFileMappingA((HANDLE)0xFFFFFFFF, &attributes, 4u, 0, strlen(decryptedEmperorDatData) + 1, 0);
  release_assert(globalCommsFileMappingHandle && GetLastError() == 0);
}

std::wstring getFolderThisExeIsIn()
{
  std::wstring path;
  path.resize(4096);
  while (true)
  {
    SetLastError(0);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    DWORD err = GetLastError();

    if (err == ERROR_INSUFFICIENT_BUFFER)
    {
      path.resize(path.size() * 2);
      continue;
    }

    release_assert(err == 0);
    path.resize(size);
    break;
  }

  // Strip the exe filename (and the trailing separator) to get the containing folder.
  // Don't hardcode the exe name here - it breaks if the exe is renamed.
  size_t lastSlash = path.find_last_of(L"\\/");
  if (lastSlash != std::wstring::npos)
    path.resize(lastSlash);
  return path;
}

std::optional<std::string> wideToAscii(const std::wstring& wide)
{
  std::string ascii;
  ascii.resize(wide.size());
  for (size_t i = 0; i < wide.size(); i++)
  {
    if (wide[i] > 127)
      return std::nullopt;
    ascii[i] = char(wide[i]);
  }
  return ascii;
}

PROCESS_INFORMATION runGameExe()
{
  // Wine/Proton/CrossOver: allow per-user type library registration (see PatchWol.cpp).
  SetEnvironmentVariableA("OAPERUSERTLIBREG", "1");

  std::string hookDllPath = *wideToAscii(getFolderThisExeIsIn()) + "\\EmperorHooks.dll";
  std::string commandLine = "Game.exe -w";
  PROCESS_INFORMATION processInfo = {};

  STARTUPINFOA startupInfo = {};
  startupInfo.cb = sizeof(STARTUPINFOA);

  release_assert(DetourCreateProcessWithDllA(0, (char*)commandLine.c_str(), 0, 0, 1, 0, 0, 0, &startupInfo, &processInfo, hookDllPath.c_str(), nullptr));

  return processInfo;
}

void sendEmperorDatDataToGameProcessViaAnonymousMapping(HANDLE processHandle, DWORD idThread)
{
  void* commsMapping = MapViewOfFileEx(globalCommsFileMappingHandle, 0xF001Fu, 0, 0, 0, 0);
  release_assert(commsMapping);
  memcpy(commsMapping, decryptedEmperorDatData, strlen(decryptedEmperorDatData) + 1);
  UnmapViewOfFile(commsMapping);

  HANDLE gameReadyEvent = CreateEventA(0, 0, 0, globalCommsEventNameGuid);
  release_assert(gameReadyEvent && GetLastError() != ERROR_ALREADY_EXISTS);

  HANDLE Handles[2];
  Handles[0] = gameReadyEvent;
  Handles[1] = processHandle;
  if (!WaitForMultipleObjects(2u, Handles, 0, 0x493E0u) && globalCommsFileMappingHandle)
    PostThreadMessageA(idThread, 0xBEEFu, 0, (LPARAM)globalCommsFileMappingHandle);
  CloseHandle(gameReadyEvent);
}

void waitForExit(PROCESS_INFORMATION* gameRunData, DWORD* exitCode)
{
  WaitForSingleObject(gameRunData->hProcess, 0xFFFFFFFF);
  if (exitCode)
    *exitCode = -1;
  if (exitCode)
    GetExitCodeProcess(gameRunData->hProcess, exitCode);
}


void writeGraphicsSettings(int screenWidth, int screenHeight)
{
  auto setValue = [&](HKEY hkey, const char* subKey, const char* valueName, const std::string& value, bool force = false)
  {
    if (!force)
    {
      LSTATUS err = RegGetValueA(hkey, subKey, valueName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr);
      if (err == ERROR_SUCCESS)
        return;
    }
    release_assert(RegSetKeyValueA(hkey, subKey, valueName, REG_SZ, value.c_str(), DWORD(value.size() + 1)) == ERROR_SUCCESS);
  };

  // force set width and height, the rest will we be set only if they don't exist
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "ScreenWidth", std::to_string(screenWidth), true);
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "ScreenHeight", std::to_string(screenHeight), true);

  // Just copied from the registry after cranking all settings in the in game settings dialogue
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "GraphicsLOD", "4");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "ColorDepth", "32");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "Shadows", "1");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "ModelLOD", "2");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "TextureLOD", "0");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "TerrainLOD", "2");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "EffectLOD", "2");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "MultiTexture", "1");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "HardwareTL", "1");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "AltDevice", "0");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "ShadowQuality", "2");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "LimitFrameRate", "0"); // this doesn't work, just btw
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Graphics", "LimitTo16BitTex", "0");

  // default to WOL when clicking multiplayer, don't offer LAN
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Multiplayer", "AutoLoginChoice", "NoAutoWolRadioButton");
  setValue(HKEY_CURRENT_USER, "Software\\WestwoodRedirect\\Emperor\\Options\\Multiplayer", "LastLoginNickname", "player");
}

int getMainMonitorHeight()
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);

  const POINT ptZero = { 0, 0 };
  HMONITOR monitor = MonitorFromPoint(ptZero, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info = {};
  info.cbSize = sizeof(MONITORINFO);
  if (monitor && GetMonitorInfo(monitor, &info))
    return info.rcMonitor.bottom - info.rcMonitor.top;

  return GetSystemMetrics(SM_CYSCREEN); // fallback if the monitor query fails
}



// Selectable game resolutions. The 4:3 ones are the original ratio the game was built around.
// The "(widescreen)" ones are 16:9: picking one turns on the widescreen projection fix in the
// hooks so the battlefield renders properly wide instead of stretched. Width/height of 0 is a
// special "match the desktop" entry (the old behaviour: sharpest / fills the screen natively, but
// the in-game fonts get small on big monitors).
// The "Upscale ... big UI" entries keep the desktop at its native size and render at a lower 16:9
// resolution, stretching the frame up to fill the screen on present (like dgVoodoo). That keeps the
// in-game fonts large and readable on a big monitor without switching the display mode. These are the
// cleanest answer to "text too small at high resolution".
// The K tier is the render resolution, not the screen: a lower render gives a bigger (softer) UI, a
// higher render gives a sharper (smaller) UI. They are deliberately kept below true 4K, since rendering
// at the screen's own resolution would be a 1:1 present and bring the tiny fonts straight back. Pick the
// tier at or below your monitor: 4K render on a 4K screen would not upscale.
struct Resolution { int width; int height; bool widescreen; bool upscale; const wchar_t* label; const wchar_t* labelDe; };
const Resolution resolutions[] =
{
  { 640, 480, false, false, L"640 x 480", L"640 x 480" },
  { 800, 600, false, false, L"800 x 600", L"800 x 600" },
  { 1024, 768, false, false, L"1024 x 768", L"1024 x 768" },
  { 1152, 864, false, false, L"1152 x 864", L"1152 x 864" },
  { 1280, 720, true, false, L"1280 x 720 (widescreen)", L"1280 x 720 (Breitbild)" },
  { 1600, 900, true, false, L"1600 x 900 (widescreen)", L"1600 x 900 (Breitbild)" },
  { 1920, 1080, true, false, L"1920 x 1080 (widescreen)", L"1920 x 1080 (Breitbild)" },
  { 2560, 1440, true, false, L"2560 x 1440 (widescreen)", L"2560 x 1440 (Breitbild)" },
  { 2560, 1440, true, true, L"Upscale 4K, big UI", L"Hochskaliert 4K, gro\u00dfe UI" },
  { 1920, 1080, true, true, L"Upscale 2K, big UI", L"Hochskaliert 2K, gro\u00dfe UI" },
  { 1280, 720, true, true, L"Upscale 1K, big UI", L"Hochskaliert 1K, gro\u00dfe UI" },
  { 0, 0, false, false, L"Desktop", L"Desktop" },
};


WNDPROC defWndProc = nullptr;
HWND window = nullptr;

// --- Look & feel ---------------------------------------------------------------------------------
UINT gDpi = 96;
inline int S(int v) { return MulDiv(v, int(gDpi), 96); } // scale 96-DPI layout pixels to the system DPI

HFONT gUiFont = nullptr;
HFONT gLinkFont = nullptr;
HWND gHeaderPic = nullptr;
HBITMAP gHeaderBmp = nullptr;

void drawHeader(const DRAWITEMSTRUCT* item)
{
  if (!gHeaderBmp)
    return;
  BITMAP bm = {};
  GetObjectW(gHeaderBmp, sizeof(bm), &bm);
  HDC mem = CreateCompatibleDC(item->hDC);
  HGDIOBJ old = SelectObject(mem, gHeaderBmp);
  SetStretchBltMode(item->hDC, HALFTONE);
  SetBrushOrgEx(item->hDC, 0, 0, nullptr);
  const RECT& rc = item->rcItem;
  StretchBlt(item->hDC, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
  SelectObject(mem, old);
  DeleteDC(mem);
}

HFONT makeFont(int pointSize, int weight, bool underline = false)
{
  return CreateFontW(-MulDiv(pointSize, int(gDpi), 72), 0, 0, 0, weight, FALSE, underline, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

BOOL CALLBACK setFontOnChild(HWND child, LPARAM font)
{
  SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
  return TRUE;
}

// Layout of the window (96-DPI pixels, scaled with S()): header band on top, navigation on the left,
// the chosen page on the right, and a bottom bar with the status line and "Play".
const int kClientW = 920, kClientH = 600;
const int kHeaderH = 84, kSidebarW = 200, kBottomBarY = 532;
const int kContentX = 224, kContentW = 680;

// Window background: header band (same gradient as the banner), sidebar, page, line above the bottom bar.
void paintBackground(HWND hwnd, HDC dc)
{
  const ui::Palette& p = ui::pal();
  RECT rc;
  GetClientRect(hwnd, &rc);
  FillRect(dc, &rc, ui::brushes().window);
  RECT side = { 0, S(kHeaderH), S(kSidebarW), rc.bottom };
  FillRect(dc, &side, ui::brushes().sidebar);
  RECT sep = { S(kSidebarW) + S(24), S(kBottomBarY), rc.right - S(16), S(kBottomBarY) + 1 };
  ui::fillRect(dc, sep, p.border);
  for (int y = 0; y < S(kHeaderH); y++)
  {
    float t = float(y) / float(std::max(1, S(kHeaderH) - 1));
    RECT line = { rc.left, y, rc.right, y + 1 };
    ui::fillRect(dc, line, RGB(int(30 - 18 * t), int(20 - 11 * t), int(16 - 8 * t)));
  }
}

HWND fullscreenCheckbox = nullptr;
HWND resolutionCombo = nullptr;
HWND hostGameRadio = nullptr;
HWND portsLabel = nullptr;
HWND connectToServerRadio = nullptr;
HWND serverAddressLabel = nullptr;
HWND serverAddressTextbox = nullptr;

HWND playButton = nullptr;
HWND testNetworkButton = nullptr;

HWND githubLink = nullptr;
HWND facebookLink = nullptr;

HWND pauseOnStartupCheckbox = nullptr;
HWND forceCursorVisibleCheckbox = nullptr;
HWND disableCursorCaptureCheckbox = nullptr;
HWND debugLogCheckbox = nullptr;
HWND cutscene43Checkbox = nullptr;
HWND smoothScrollCheckbox = nullptr;
HWND languageCombo = nullptr;
HWND graphicsCombo = nullptr; // Off / Modern / HD
HWND cameraZoomCombo = nullptr;

// Farthest camera zoom choices (percent of the original game's farthest zoom)
const int cameraZoomSteps[] = { 100, 125, 150, 175 };
HWND shortcutButton = nullptr;
HWND moneyCheatCheckbox = nullptr;
HWND groupMoveCheckbox = nullptr;
HWND radarCalmCheckbox = nullptr;
HWND mpSettingsLabel = nullptr;
HWND largeFontCheckbox = nullptr;
HWND modsCheckbox = nullptr;
HWND modsFolderButton = nullptr;
HWND hdTexturesCheckbox = nullptr;
HWND hdTexturesButton = nullptr;
HWND hdTexturesStatus = nullptr;
HWND themeButton = nullptr;
HWND statusLabel = nullptr;
HWND versionLabel = nullptr;
HFONT gHeadingFont = nullptr, gTitleFont = nullptr, gPlayFont = nullptr, gBoldFont = nullptr;

// Pages of the window: navigation button + the controls shown on that page.
enum PageId { PageStart, PageGraphics, PageMultiplayer, PageMods, PageCompat, PageLog, PageCount };
HWND gNavButtons[PageCount] = {};
std::vector<HWND> gPageControls[PageCount];
int gCurrentPage = PageStart;
std::vector<HWND> gMutedLabels;   // explanations under options (grey)
std::vector<HWND> gSidebarLabels; // version + links at the bottom of the navigation
std::vector<HWND> gThemedNative;  // combo boxes and edit fields (Windows' dark theme)

bool contains(const std::vector<HWND>& v, HWND h) { return std::find(v.begin(), v.end(), h) != v.end(); }

void showPage(int page)
{
  gCurrentPage = page;
  for (int i = 0; i < PageCount; i++)
    for (HWND c : gPageControls[i])
      ShowWindow(c, i == page ? SW_SHOW : SW_HIDE);
  for (HWND n : gNavButtons)
    InvalidateRect(n, nullptr, FALSE);
}

extern Settings settings;

void applyTheme()
{
  ui::gDark = settings.launcherTheme == 2 || (settings.launcherTheme == 0 && ui::windowsUsesDarkApps());
  ui::rebuildBrushes();
  ui::themeTitleBar(window);
  for (HWND c : gThemedNative)
    ui::themeNative(c);
  if (themeButton)
    SetWindowTextW(themeButton, ui::gDark ? T(L"Light mode", L"Helles Design") : T(L"Dark mode", L"Dunkles Design"));
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// Shown in the launcher and written to the debug logs. Bump together with package.ps1 $Version (the CI
// compares the "Emperor Spice vX.Y" text in this file with it).
const wchar_t kLauncherVersion[] = L"Emperor Spice v1.1";

// Game folder (contains DATA); set in wmain. The HD texture workshop works there.
std::filesystem::path gGameDir;

// Command line argument (custom install dir) we were started with, so a language switch can restart us identically.
std::wstring gInstallDirArg;

bool playClicked = false;

Settings settings;


void refreshDerivedState()
{
  bool checked = SendMessage(connectToServerRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
  EnableWindow(serverAddressTextbox, checked);
  EnableWindow(testNetworkButton, checked);
}


std::filesystem::path hdToolDir() { return std::filesystem::path(getFolderThisExeIsIn()) / L"hdtools"; }

// HD texture state under the switch, e.g. "Made on 15.11.2026 - 205 MB".
void refreshHdTexturesStatus()
{
  if (!hdTexturesStatus)
    return;
  unsigned long long bytes = 0;
  std::wstring date;
  std::wstring text;
  if (!hdTextures::toolAvailable(hdToolDir()))
    text = T(L"Not included in this build.", L"In dieser Version nicht enthalten.");
  else if (hdTextures::info(gGameDir, bytes, date))
    text = std::wstring(T(L"Made on ", L"Erzeugt am ")) + date + L" \u00b7 " + std::to_wstring(bytes >> 20) + L" MB";
  else
    text = T(L"Not generated yet - \"Generate\" takes about 5 to 30 minutes.",
             L"Noch nicht erzeugt - \u201eErzeugen\u201c dauert etwa 5 bis 30 Minuten.");
  SetWindowTextW(hdTexturesStatus, text.c_str());
}

// Bottom bar: the most important settings at a glance.
void refreshStatus()
{
  if (!statusLabel || !IsWindow(statusLabel))
    return;
  std::wstring res;
  for (const Resolution& r : resolutions)
    if (r.width == settings.screenWidth && r.height == settings.screenHeight && r.upscale == settings.upscaleToDesktop)
      res = T(r.label, r.labelDe);
  const wchar_t* style = settings.graphicsPack ? (settings.graphicsStyle == 1 ? L"HD" : L"Modern") : L"Original";
  std::wstring text = res + L"  \u00b7  " + T(L"Look: ", L"Grafikstil: ") + style + L"  \u00b7  " + T(L"HD textures ", L"HD-Texturen ") +
                      (settings.hdTextures ? T(L"on", L"an") : T(L"off", L"aus"));
  if (settings.mods)
    text += std::wstring(L"  \u00b7  ") + T(L"Mods on", L"Mods an");
  if (!settings.hostGame)
    text += std::wstring(L"  \u00b7  ") + T(L"Multiplayer: client", L"Mehrspieler: Client");
  SetWindowTextW(statusLabel, text.c_str());
  refreshHdTexturesStatus();

  // Multiplayer page: what all players need to have the same (otherwise the games drift apart)
  if (mpSettingsLabel)
  {
    std::wstring mp = std::wstring(kLauncherVersion) + L"\n" +
                      T(L"Group movement: ", L"Gruppenbewegung: ") + (settings.groupMove ? T(L"on", L"an") : T(L"off", L"aus")) + L"\n" +
                      T(L"Mods: ", L"Mods: ") + (settings.mods ? T(L"on (everyone needs the same mods)", L"an (alle brauchen dieselben Mods)") : T(L"off", L"aus"));
    SetWindowTextW(mpSettingsLabel, mp.c_str());
  }
}

void refreshHdTexturesButton()
{
  SetWindowTextW(hdTexturesButton, hdTextures::generated(gGameDir) ? T(L"Make again\u2026", L"Neu erzeugen\u2026")
                                                                   : T(L"Generate\u2026", L"Erzeugen\u2026"));
}

// Asks, then generates the HD textures with a progress window. true = textures are ready.
bool generateHdTextures()
{
  const bool again = hdTextures::generated(gGameDir);
  std::wstring question = again
    ? T(L"Make the HD textures again from your game files?\n\nThis replaces the current HD textures.",
        L"HD-Texturen neu aus deinen Spieldateien erzeugen?\n\nDie bisherigen HD-Texturen werden dabei ersetzt.")
    : T(L"The graphics workshop now upscales about 1,600 textures of your game with AI (Real-ESRGAN): "
        L"build icons, units, buildings and terrain. Effects stay original.\n\n"
        L"This takes about 5 to 30 minutes, depending on your graphics card, and needs about 250 MB of disk space. "
        L"You can cancel at any time.\n\nStart now?",
        L"Die Grafik-Werkstatt rechnet jetzt rund 1.600 Texturen deines Spiels mit KI hoch (Real-ESRGAN): "
        L"Bau-Icons, Einheiten, Geb\u00e4ude und Boden. Effekte bleiben original.\n\n"
        L"Das dauert je nach Grafikkarte etwa 5 bis 30 Minuten und braucht rund 250 MB Platz. "
        L"Du kannst jederzeit abbrechen.\n\nJetzt starten?");
  if (MessageBoxW(window, question.c_str(), T(L"Graphics workshop", L"Grafik-Werkstatt"), MB_YESNO | MB_ICONQUESTION) != IDYES)
    return false;

  launcherLog::write("Graphics workshop: generating HD textures");
  const DWORD started = GetTickCount();
  std::wstring error = hdTexturesUi::run(window, gUiFont, S, hdToolDir(), gGameDir);
  launcherLog::write("Graphics workshop: %s after %lu s", error.empty() ? "done" : launcherLog::utf8(error).c_str(),
                     (GetTickCount() - started) / 1000);
  refreshHdTexturesButton();
  if (error == L"cancelled")
    return false;
  if (!error.empty())
  {
    MessageBoxW(window, (T(L"The HD textures could not be generated.\n\n", L"Die HD-Texturen konnten nicht erzeugt werden.\n\n") + error).c_str(),
                T(L"Graphics workshop", L"Grafik-Werkstatt"), MB_OK | MB_ICONWARNING);
    return false;
  }
  MessageBoxW(window, T(L"Done! The HD textures are switched on. You can switch them off at any time with the tick box.",
                        L"Fertig! Die HD-Texturen sind eingeschaltet. Du kannst sie jederzeit \u00fcber das H\u00e4kchen wieder ausschalten."),
              T(L"Graphics workshop", L"Grafik-Werkstatt"), MB_OK | MB_ICONINFORMATION);
  return true;
}

void setHdTexturesOn(bool on)
{
  settings.hdTextures = on;
  settings.writeSettings();
  SendMessageA(hdTexturesCheckbox, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

void loadAndApplySettings()
{
  settings.readSettings();

  SendMessageA(fullscreenCheckbox, BM_SETCHECK, settings.fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);

  int resolutionIndex = 4; // default to 1280x720 (lowest widescreen) if the saved value isn't in the list
  for (int i = 0; i < int(std::size(resolutions)); i++)
  {
    // Match on upscale too, otherwise the native and "(upscale, big UI)" entries that share a
    // resolution (e.g. 1920x1080) are indistinguishable and the wrong one gets selected.
    if (resolutions[i].width == settings.screenWidth && resolutions[i].height == settings.screenHeight
        && resolutions[i].upscale == settings.upscaleToDesktop)
    {
      resolutionIndex = i;
      break;
    }
  }
  SendMessageA(resolutionCombo, CB_SETCURSEL, resolutionIndex, 0);

  SendMessageA(hostGameRadio, BM_SETCHECK, settings.hostGame ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(connectToServerRadio, BM_SETCHECK, settings.hostGame ? BST_UNCHECKED : BST_CHECKED, 0);
  SendMessageA(serverAddressTextbox, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(settings.serverAddress.c_str()));
  SendMessageA(pauseOnStartupCheckbox, BM_SETCHECK, settings.pauseOnStartup ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(forceCursorVisibleCheckbox, BM_SETCHECK, settings.forceCursorVisible ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(disableCursorCaptureCheckbox, BM_SETCHECK, settings.disableCursorCapture ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(debugLogCheckbox, BM_SETCHECK, settings.debugLog ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(cutscene43Checkbox, BM_SETCHECK, settings.cutscene43 ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(smoothScrollCheckbox, BM_SETCHECK, settings.smoothScroll ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(graphicsCombo, CB_SETCURSEL, !settings.graphicsPack ? 0 : (settings.graphicsStyle == 1 ? 2 : 1), 0);
  {
    int zoomIndex = 0;
    for (int i = 0; i < int(std::size(cameraZoomSteps)); i++)
      if (cameraZoomSteps[i] <= settings.cameraZoom)
        zoomIndex = i;
    SendMessageA(cameraZoomCombo, CB_SETCURSEL, zoomIndex, 0);
  }
  SendMessageA(moneyCheatCheckbox, BM_SETCHECK, settings.moneyCheat ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(groupMoveCheckbox, BM_SETCHECK, settings.groupMove ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(radarCalmCheckbox, BM_SETCHECK, settings.radarCalm ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(largeFontCheckbox, BM_SETCHECK, settings.largeFont ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(modsCheckbox, BM_SETCHECK, settings.mods ? BST_CHECKED : BST_UNCHECKED, 0);
  // Without generated textures the box can't be on (e.g. after a reinstall of the game).
  if (settings.hdTextures && !hdTextures::generated(gGameDir))
    settings.hdTextures = false;
  SendMessageA(hdTexturesCheckbox, BM_SETCHECK, settings.hdTextures ? BST_CHECKED : BST_UNCHECKED, 0);
  refreshHdTexturesButton();

  refreshDerivedState();
}


LRESULT onServerOrClientRadioClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  settings.hostGame = SendMessage(hostGameRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();

  refreshDerivedState();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onFullscreenCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.fullscreen = SendMessage(fullscreenCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();

  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onCutscene43CheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.cutscene43 = SendMessage(cutscene43Checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();

  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onSmoothScrollCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.smoothScroll = SendMessage(smoothScrollCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();

  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onCameraZoomChanged(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  int index = int(SendMessageA(cameraZoomCombo, CB_GETCURSEL, 0, 0));
  if (index >= 0 && index < int(std::size(cameraZoomSteps)))
  {
    settings.cameraZoom = cameraZoomSteps[index];
    settings.writeSettings();
  }
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onGraphicsChanged(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  int index = int(SendMessageA(graphicsCombo, CB_GETCURSEL, 0, 0));
  if (index >= 0 && index <= 2)
  {
    settings.graphicsPack = index != 0;
    if (index != 0)
      settings.graphicsStyle = index == 2 ? 1 : 0;
    settings.writeSettings();
  }
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onHdTexturesCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  bool on = SendMessage(hdTexturesCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  // Switched on but nothing generated yet: offer to do it now. The player decides.
  if (on && !hdTextures::generated(gGameDir))
    on = generateHdTextures();
  setHdTexturesOn(on);
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onHdTexturesButtonClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  if (generateHdTextures())
    setHdTexturesOn(true);
  else if (!hdTextures::generated(gGameDir))
    setHdTexturesOn(false);
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onModsCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.mods = SendMessage(modsCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT OnModsFolderClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  std::filesystem::path modsDir = std::filesystem::path(getFolderThisExeIsIn()) / L"mods";
  std::error_code ec;
  std::filesystem::create_directories(modsDir, ec);
  ShellExecuteW(window, L"open", modsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onMoneyCheatCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.moneyCheat = SendMessage(moneyCheatCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onRadarCalmCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.radarCalm = SendMessage(radarCalmCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onGroupMoveCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.groupMove = SendMessage(groupMoveCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onLargeFontCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.largeFont = SendMessage(largeFontCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onResolutionChanged(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  int index = int(SendMessageA(resolutionCombo, CB_GETCURSEL, 0, 0));
  if (index >= 0 && index < int(std::size(resolutions)))
  {
    settings.screenWidth = resolutions[index].width;
    settings.screenHeight = resolutions[index].height;
    settings.widescreen = resolutions[index].widescreen;
    settings.upscaleToDesktop = resolutions[index].upscale;
    // The small in-game messages are drawn in fixed pixels, so they get tiny at high resolutions.
    // From 1440p (rendered natively, not upscaled) suggest the 2x message font. The player can still
    // change it afterwards.
    if (IsWindowEnabled(largeFontCheckbox))
    {
      settings.largeFont = !resolutions[index].upscale && resolutions[index].height >= 1440;
      SendMessage(largeFontCheckbox, BM_SETCHECK, settings.largeFont ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    settings.writeSettings();
  }
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

std::string GetText(HWND hwnd)
{
  size_t size = SendMessageA(hwnd, WM_GETTEXTLENGTH, 0, 0);
  if (!size)
    return "";
  std::string text(size, '\0');
  SendMessageA(hwnd, WM_GETTEXT, text.size() + 1, reinterpret_cast<LPARAM>(text.c_str()));
  return text;
}

LRESULT onServerAddressTextChanged(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.serverAddress = GetText(serverAddressTextbox);
  settings.writeSettings();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onPauseOnStartupCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.pauseOnStartup = SendMessage(pauseOnStartupCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onForceCursorVisibleCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.forceCursorVisible = SendMessage(forceCursorVisibleCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onDisableCursorCaptureCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.disableCursorCapture = SendMessage(disableCursorCaptureCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT onDebugLogCheckboxClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  settings.debugLog = SendMessage(debugLogCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings.writeSettings();
  launcherLog::open(gGameDir, settings.debugLog); // off: launcher.txt is removed right away
  launcherLog::write("Debug log switched on");
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

// Switching the language restarts the launcher, so every label is simply recreated in the new language.
LRESULT onLanguageChanged(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  int index = int(SendMessageA(languageCombo, CB_GETCURSEL, 0, 0));
  Lang chosen = index == 1 ? Lang::DE : Lang::EN;
  if (chosen != gLang)
  {
    settings.language = languageToSetting(chosen);
    settings.writeSettings();

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring args = gInstallDirArg.empty() ? std::wstring() : (L"\"" + gInstallDirArg + L"\"");
    ShellExecuteW(nullptr, L"open", exePath, args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);

    DestroyWindow(window);
    PostQuitMessage(0);
    return 0;
  }
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT OnWindowClose(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  PostQuitMessage(0);
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT OnPlayClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  playClicked = true;
  DestroyWindow(window);
  PostQuitMessage(0);
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT OnShortcutClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  if (createDesktopShortcut())
    MessageBoxW(window, T(L"The shortcut \"Emperor Spice\" was created on your desktop.", L"Die Verkn\u00fcpfung \"Emperor Spice\" wurde auf dem Desktop angelegt."), L"Emperor Spice", MB_OK | MB_ICONINFORMATION);
  else
    MessageBoxW(window, T(L"The desktop shortcut could not be created.", L"Die Desktop-Verkn\u00fcpfung konnte nicht angelegt werden."), L"Emperor Spice", MB_OK | MB_ICONWARNING);
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT OnTestNetworkClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  testNetwork(window, settings.serverAddress);
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}


LRESULT onLinkClicked(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, const char* url)
{
  ShellExecuteA(window, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

LRESULT handleCommand(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  if (hwnd == window && message == WM_ERASEBKGND)
  {
    paintBackground(hwnd, reinterpret_cast<HDC>(wParam));
    return 1;
  }
  if (hwnd == window && (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN))
  {
    HDC dc = reinterpret_cast<HDC>(wParam);
    HWND control = reinterpret_cast<HWND>(lParam);
    const ui::Palette& p = ui::pal();
    SetBkMode(dc, TRANSPARENT);
    COLORREF text = p.text;
    if (control == githubLink || control == facebookLink)
      text = p.link;
    else if (contains(gMutedLabels, control) || !IsWindowEnabled(control))
      text = p.muted;
    SetTextColor(dc, text);
    return reinterpret_cast<LRESULT>(contains(gSidebarLabels, control) ? ui::brushes().sidebar : ui::brushes().window);
  }
  if (hwnd == window && (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX))
  {
    HDC dc = reinterpret_cast<HDC>(wParam);
    SetTextColor(dc, ui::pal().text);
    SetBkColor(dc, ui::pal().control);
    return reinterpret_cast<LRESULT>(ui::brushes().control);
  }
  if (hwnd == window && message == WM_DRAWITEM)
  {
    const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
    if (item->hwndItem == gHeaderPic)
    {
      drawHeader(item);
      return TRUE;
    }
    ui::ButtonKind kind = ui::ButtonKind::Normal;
    if (item->hwndItem == playButton)
      kind = ui::ButtonKind::Primary;
    else if (item->hwndItem == themeButton)
      kind = ui::ButtonKind::Toggle;
    for (int i = 0; i < PageCount; i++)
      if (item->hwndItem == gNavButtons[i])
        kind = i == gCurrentPage ? ui::ButtonKind::NavSelected : ui::ButtonKind::Nav;
    ui::drawButton(item, kind, ui::hotButton() == item->hwndItem);
    return TRUE;
  }
  if (hwnd == window && message == WM_SETCURSOR)
  {
    HWND over = reinterpret_cast<HWND>(wParam);
    if (over == githubLink || over == facebookLink)
    {
      SetCursor(LoadCursor(nullptr, IDC_HAND));
      return TRUE;
    }
  }
  if (hwnd == window && message == WM_SETTINGCHANGE && settings.launcherTheme == 0)
    applyTheme(); // Windows switched between light and dark

  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED)
  {
    HWND from = reinterpret_cast<HWND>(lParam);
    for (int i = 0; i < PageCount; i++)
      if (from == gNavButtons[i])
      {
        showPage(i);
        return 0;
      }
    if (from == themeButton)
    {
      settings.launcherTheme = ui::gDark ? 1 : 2;
      settings.writeSettings();
      applyTheme();
      return 0;
    }
  }

  if (message == WM_COMMAND && hwnd == window)
  {
    LRESULT result = handleCommand(hwnd, message, wParam, lParam);
    refreshStatus(); // keep the bottom bar in step with every change
    return result;
  }
  return handleCommand(hwnd, message, wParam, lParam);
}

LRESULT handleCommand(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  if (message == WM_CLOSE && hwnd == window) return OnWindowClose(hwnd, message, wParam, lParam);

  if (message == WM_COMMAND && HIWORD(wParam) == STN_CLICKED && reinterpret_cast<HWND>(lParam) == githubLink) return onLinkClicked(hwnd, message, wParam, lParam, "https://github.com/azmawee/EmperorReborn");
  if (message == WM_COMMAND && HIWORD(wParam) == STN_CLICKED && reinterpret_cast<HWND>(lParam) == facebookLink) return onLinkClicked(hwnd, message, wParam, lParam, "https://github.com/badrig217/EmperorSpice");

  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == fullscreenCheckbox) return onFullscreenCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == cutscene43Checkbox) return onCutscene43CheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == CBN_SELCHANGE && reinterpret_cast<HWND>(lParam) == graphicsCombo) return onGraphicsChanged(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == smoothScrollCheckbox) return onSmoothScrollCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == CBN_SELCHANGE && reinterpret_cast<HWND>(lParam) == languageCombo) return onLanguageChanged(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == CBN_SELCHANGE && reinterpret_cast<HWND>(lParam) == resolutionCombo) return onResolutionChanged(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == CBN_SELCHANGE && reinterpret_cast<HWND>(lParam) == cameraZoomCombo) return onCameraZoomChanged(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == hostGameRadio) return onServerOrClientRadioClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == connectToServerRadio) return onServerOrClientRadioClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == EN_CHANGE && reinterpret_cast<HWND>(lParam) == serverAddressTextbox) return onServerAddressTextChanged(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == playButton) return OnPlayClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == pauseOnStartupCheckbox) return onPauseOnStartupCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == forceCursorVisibleCheckbox) return onForceCursorVisibleCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == disableCursorCaptureCheckbox) return onDisableCursorCaptureCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == debugLogCheckbox) return onDebugLogCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == moneyCheatCheckbox) return onMoneyCheatCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == groupMoveCheckbox) return onGroupMoveCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == radarCalmCheckbox) return onRadarCalmCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == largeFontCheckbox) return onLargeFontCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == shortcutButton) return OnShortcutClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == hdTexturesCheckbox) return onHdTexturesCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == hdTexturesButton) return onHdTexturesButtonClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == modsCheckbox) return onModsCheckboxClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == modsFolderButton) return OnModsFolderClicked(hwnd, message, wParam, lParam);
  if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == testNetworkButton) return OnTestNetworkClicked(hwnd, message, wParam, lParam);

  return CallWindowProc(defWndProc, hwnd, message, wParam, lParam);
}

std::string getMd5OfFile(const std::string& path)
{
  FILE* f = fopen(path.c_str(), "rb");
  if (!f)
    return "";

  release_assert(fseek(f, 0, SEEK_END) == 0);
  long size = ftell(f);
  release_assert(size != -1);
  release_assert(fseek(f, 0, SEEK_SET) == 0);

  std::vector<uint8_t> data(size);
  release_assert(fread(data.data(), 1, size, f) == size);
  fclose(f);


  MD5_CTX md5 = {};
  MD5Init(&md5);
  MD5Update(&md5, data.data(), uint32_t(data.size()));
  unsigned char md5Bytes[16];
  MD5Final(md5Bytes, &md5);

  std::string md5Hex;
  md5Hex.resize(32);

  for (int32_t i = 0; i < 16; i++)
    sprintf(md5Hex.data() + i*2, "%02x", md5Bytes[i]);

  return md5Hex;
}


// Adds every .iso in the given folders whose volume label is one of the four Emperor discs and that is
// not assigned yet. So picking a single image is enough when all four lie in the same folder.
void addIsoImagesFromFolders(const std::vector<std::wstring>& files, std::map<std::string, std::wstring>& images)
{
  std::vector<std::filesystem::path> folders;
  for (const std::wstring& file : files)
  {
    std::filesystem::path folder = std::filesystem::path(file).parent_path();
    if (std::find(folders.begin(), folders.end(), folder) == folders.end())
      folders.push_back(folder);
  }
  for (const auto& folder : folders)
  {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    {
      std::wstring ext = entry.path().extension().wstring();
      for (auto& c : ext) c = towlower(c);
      if (!entry.is_regular_file(ec) || ext != L".iso")
        continue;
      std::string label = iso::emperorDisc(entry.path());
      if (!label.empty() && !images.count(label))
        images[label] = entry.path().wstring();
    }
  }
}

// Lets the player pick the ISO images of the 4 discs. Each file is identified by its volume label
// (EMPEROR1..EMPEROR4), so the order and file names do not matter. Discs without an image are read
// from a CD drive later. Returns false if the player cancelled.
bool selectIsoImages(std::map<std::string, std::wstring>& images)
{
  while (true)
  {
    std::vector<wchar_t> buffer(64 * 1024, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"ISO (*.iso)\0*.iso\0*.*\0*.*\0\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = DWORD(buffer.size());
    ofn.lpstrTitle = T(L"Select an ISO image of the game (the other discs in the same folder are found automatically)",
                       L"Ein ISO-Abbild des Spiels ausw\u00e4hlen (die anderen CDs im selben Ordner werden automatisch gefunden)");
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn))
      return false;

    // Multi-select result: "folder\0file1\0file2\0\0", single file: "full path\0\0".
    std::vector<std::wstring> files;
    std::wstring first = buffer.data();
    const wchar_t* p = buffer.data() + first.size() + 1;
    if (*p == L'\0')
      files.push_back(first);
    else
      for (; *p; p += wcslen(p) + 1)
        files.push_back(first + L"\\" + p);

    images.clear();
    std::wstring unknown;
    for (const std::wstring& file : files)
    {
      std::string label = iso::emperorDisc(std::filesystem::path(file));
      if (!label.empty())
        images[label] = file;
      else
        unknown += L"\n  " + std::filesystem::path(file).filename().wstring();
    }
    addIsoImagesFromFolders(files, images);

    std::wstring summary;
    for (int i = 1; i <= 4; i++)
    {
      std::string label = "EMPEROR" + std::to_string(i);
      auto it = images.find(label);
      summary += (gLang == Lang::DE ? L"\nCD " : L"\nDisc ") + std::to_wstring(i) + L": " +
                 (it != images.end() ? std::filesystem::path(it->second).filename().wstring()
                                     : std::wstring(T(L"- (will be asked for as a CD)", L"- (wird als CD abgefragt)")));
    }
    if (!unknown.empty())
      summary += std::wstring(T(L"\n\nNot an Emperor disc image:", L"\n\nKein Emperor-CD-Abbild:")) + unknown;

    int choice = MessageBoxW(nullptr, (std::wstring(T(L"Found these disc images:\n", L"Gefundene CD-Abbilder:\n")) + summary +
                                       T(L"\n\nContinue = install,  Try Again = choose again,  Cancel = quit",
                                         L"\n\nWeiter = installieren,  Wiederholen = neu ausw\u00e4hlen,  Abbrechen = beenden")).c_str(),
                             L"Emperor Spice", (images.empty() ? MB_RETRYCANCEL : MB_CANCELTRYCONTINUE) | MB_ICONQUESTION);
    if (choice == IDCANCEL)
      return false;
    if (choice == IDCONTINUE && !images.empty())
      return true;
    // IDRETRY / IDTRYAGAIN: pick again
  }
}

// Asks how to install: from CDs (or images already mounted in Windows) or from ISO files.
// Returns false if the player cancelled; isoImages is filled when ISO files were chosen.
bool chooseInstallSource(std::map<std::string, std::wstring>& isoImages)
{
  int choice = MessageBoxW(nullptr, T(
      L"How do you want to install the game?\n\n"
      L"Yes - from the CDs (or disc images mounted in Windows)\n"
      L"No - from ISO files (no mounting needed)\n"
      L"Cancel - quit",
      L"Wie m\u00f6chtest du das Spiel installieren?\n\n"
      L"Ja - von den CDs (oder in Windows eingebundenen Abbildern)\n"
      L"Nein - aus ISO-Dateien (ohne Einbinden)\n"
      L"Abbrechen - beenden"),
      L"Emperor Spice", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDCANCEL)
    return false;
  isoImages.clear();
  if (choice == IDNO)
    return selectIsoImages(isoImages);
  return true;
}

int wmain(int argc, wchar_t* argv[])
{
  std::wstring installDirUnicode;
  // Crisp rendering on scaled displays: lay out in real pixels for the system DPI.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
  gDpi = GetDpiForSystem();

  // Pick the UI language first, so every message from here on (installer included) is translated.
  settings.readSettings();
  gLang = languageFromSetting(settings.language);

  if (argc > 1)
  {
    installDirUnicode = argv[1];
    gInstallDirArg = argv[1];
  }
  else
    installDirUnicode = getFolderThisExeIsIn() + L"\\data";
  gGameDir = installDirUnicode;
  launcherLog::open(gGameDir, settings.debugLog);
  launcherLog::write("%s launcher", launcherLog::utf8(kLauncherVersion).c_str());
  launcherLog::write("Launcher folder: %s", launcherLog::utf8(getFolderThisExeIsIn()).c_str());
  launcherLog::write("Game folder: %s", launcherLog::utf8(installDirUnicode).c_str());
  // the game's debug log reads it from here for its system profile
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\WestwoodRedirect\\Emperor\\LauncherCustomSettings", L"LauncherVersion", REG_SZ,
                  kLauncherVersion, DWORD((wcslen(kLauncherVersion) + 1) * sizeof(wchar_t)));

  // This is unfortunate, but I just don't want to deal with debugging it.
  // Pretty sure the base game won't work anyway, even if I fix my code.
  for (std::wstring item : {installDirUnicode, getFolderThisExeIsIn()})
  {
    if (!wideToAscii(item).has_value())
    {
      MessageBoxW(nullptr, (item + T(L" contains non-English characters (e.g. umlauts), please move to a folder with only English characters",
                                       L" enth\u00e4lt Sonderzeichen (z. B. Umlaute). Bitte verschiebe alles in einen Ordner, dessen Pfad nur englische Buchstaben enth\u00e4lt.")).c_str(),
                  T(L"Bad install path", L"Ung\u00fcltiger Installationspfad"), MB_OK);
      return 1;
    }
  }

  std::string installDir = *wideToAscii(installDirUnicode);
  std::string gameExeMd5 = getMd5OfFile(installDir + "\\Game.exe");
  launcherLog::write("Game.exe MD5: %s", gameExeMd5.empty() ? "(not found)" : gameExeMd5.c_str());
  std::string_view desiredMd5 = "7dc4fda2f6b7e8a6b70e846686a81938";

  if (gameExeMd5.empty())
  {
    // No Game.exe at all - nothing to run, so we must install (or quit).
    if (MessageBoxW(nullptr, (T(L"Game.exe was not found in ", L"Game.exe wurde nicht gefunden in ") + toWide(installDir) +
                              T(L".\n\nInstall the game here now?", L".\n\nSoll das Spiel jetzt hier installiert werden?")).c_str(),
                    T(L"Game not found", L"Spiel nicht gefunden"), MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
      return 0;

    std::map<std::string, std::wstring> isoImages;
    if (!chooseInstallSource(isoImages))
      return 0;

    installEmperor(installDir, isoImages);

    if (getMd5OfFile(installDir + "\\Game.exe").empty())
    {
      MessageBoxW(nullptr, T(L"Installation failed!", L"Installation fehlgeschlagen!"), T(L"Error", L"Fehler"), MB_OK);
      return 1;
    }

    bool shortcut = createDesktopShortcut();
    MessageBoxW(nullptr, shortcut
                  ? T(L"Installation complete. A shortcut \"Emperor Spice\" was created on your desktop.",
                      L"Installation abgeschlossen. Auf dem Desktop wurde die Verkn\u00fcpfung \"Emperor Spice\" angelegt.")
                  : T(L"Installation complete.", L"Installation abgeschlossen."),
                L"Emperor Spice", MB_OK | MB_ICONINFORMATION);
  }
  else if (gameExeMd5 != desiredMd5)
  {
    // Game.exe exists but is not the original v1.09 build the patches are written for (e.g. another
    // mod's patched exe, or a different patch level). Warn, but let the user run anyway if they want.
    int choice = MessageBoxW(nullptr, T(
        L"Your Game.exe is not the original v1.09 version this launcher's features are built for "
        L"(checksum mismatch - it may be from another mod or a different patch).\n\n"
        L"Widescreen and the other tweaks may not work and the game could crash or be unstable. "
        L"You can proceed at your own risk.\n\n"
        L"Yes - run anyway (at your own risk)\n"
        L"No - (re)install the correct Game.exe\n"
        L"Cancel - quit",
        L"Deine Game.exe ist nicht die originale Version 1.09, f\u00fcr die dieser Launcher gebaut ist "
        L"(Pr\u00fcfsumme stimmt nicht - evtl. von einer anderen Mod oder einem anderen Patch).\n\n"
        L"Breitbild und die anderen Verbesserungen funktionieren dann eventuell nicht, das Spiel kann abst\u00fcrzen. "
        L"Du kannst auf eigenes Risiko fortfahren.\n\n"
        L"Ja - trotzdem starten (auf eigenes Risiko)\n"
        L"Nein - die richtige Game.exe (neu) installieren\n"
        L"Abbrechen - beenden"),
        T(L"Game.exe version mismatch", L"Falsche Game.exe-Version"), MB_YESNOCANCEL | MB_ICONWARNING);

    if (choice == IDCANCEL)
      return 0;
    if (choice == IDNO)
    {
      std::map<std::string, std::wstring> isoImages;
      if (!chooseInstallSource(isoImages))
        return 0;
      installEmperor(installDir, isoImages);
      if (getMd5OfFile(installDir + "\\Game.exe") != desiredMd5)
        MessageBoxW(nullptr, T(L"Game.exe still does not match the expected version - continuing anyway.",
                               L"Die Game.exe entspricht immer noch nicht der erwarteten Version - es geht trotzdem weiter."),
                    T(L"Note", L"Hinweis"), MB_OK);
    }
    // Yes: fall through and launch with the current Game.exe.
  }

  SetCurrentDirectoryA(installDir.c_str());

  // main window
  // Layout is written in 96-DPI pixels and scaled with S() for the system DPI, so the launcher stays
  // crisp (not bitmap-stretched) on displays with Windows scaling above 100%.
  *ui::dpiPtr() = gDpi;
  ui::registerControls();
  window = CreateWindowEx(0, WC_DIALOG, L"Emperor Spice", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          0, 0, 100, 100, nullptr, nullptr, nullptr, nullptr);

  // Our own emblem (icon/EmperorSpice.ico, embedded via Resources.rc) on the title bar and taskbar.
  HICON appIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
  if (appIcon)
  {
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
  }

  gUiFont = makeFont(9, FW_NORMAL);
  gLinkFont = makeFont(9, FW_NORMAL, true);
  gBoldFont = makeFont(9, FW_SEMIBOLD);
  gHeadingFont = makeFont(11, FW_SEMIBOLD);
  gTitleFont = makeFont(16, FW_SEMIBOLD);
  gPlayFont = makeFont(12, FW_SEMIBOLD);

  // Branding header banner (icon/launcher-header.bmp, generated by icon/generate_spice.py), drawn scaled
  // with HALFTONE filtering (see WM_DRAWITEM) so it stays sharp at any Windows display scaling.
  gHeaderPic = CreateWindowEx(0, WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, S(16), S(8), S(296), S(68), window, nullptr, nullptr, nullptr);
  gHeaderBmp = reinterpret_cast<HBITMAP>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDB_HEADER), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));

  // light / dark switch, remembered in the settings
  themeButton = CreateWindowEx(0, WC_BUTTON, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               S(kClientW - 16 - 140), S(26), S(140), S(32), window, nullptr, nullptr, nullptr);
  ui::trackHover(themeButton);

  // --- helpers to build the pages -----------------------------------------------------------------
  int page = PageStart;
  auto put = [&](HWND h) { gPageControls[page].push_back(h); return h; };
  auto label = [&](const wchar_t* text, int x, int y, int w, int h) {
    return put(CreateWindowEx(0, WC_STATIC, text, WS_CHILD | SS_NOPREFIX, S(x), S(y), S(w), S(h), window, nullptr, nullptr, nullptr));
  };
  auto muted = [&](const wchar_t* text, int x, int y, int w, int h) {
    HWND l = label(text, x, y, w, h);
    gMutedLabels.push_back(l);
    return l;
  };
  auto title = [&](const wchar_t* text) {
    HWND l = label(text, kContentX, 100, kContentW, 34);
    SendMessageW(l, WM_SETFONT, reinterpret_cast<WPARAM>(gTitleFont), FALSE);
    return l;
  };
  auto heading = [&](const wchar_t* text, int y) {
    HWND l = label(text, kContentX, y, kContentW, 24);
    SendMessageW(l, WM_SETFONT, reinterpret_cast<WPARAM>(gHeadingFont), FALSE);
    return l;
  };
  auto check = [&](const wchar_t* text, int x, int y, int w, DWORD extra = 0) {
    return put(CreateWindowEx(0, ui::kCheckClass, text, WS_CHILD | WS_TABSTOP | extra, S(x), S(y), S(w), S(22), window, nullptr, nullptr, nullptr));
  };
  auto button = [&](const wchar_t* text, int x, int y, int w, int h = 30) {
    HWND b = put(CreateWindowEx(0, WC_BUTTON, text, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, S(x), S(y), S(w), S(h), window, nullptr, nullptr, nullptr));
    ui::trackHover(b);
    return b;
  };
  auto combo = [&](int x, int y, int w, int listH) {
    HWND c = put(CreateWindowEx(0, WC_COMBOBOX, nullptr, WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, S(x), S(y), S(w), S(listH), window, nullptr, nullptr, nullptr));
    gThemedNative.push_back(c);
    return c;
  };
  const int X = kContentX, X2 = kContentX + 160; // label column / control column
  const int descX = kContentX + 24, descW = kContentW - 24;

  // --- Start -------------------------------------------------------------------------------------
  page = PageStart;
  {
    title(T(L"Welcome to Emperor Spice", L"Willkommen bei Emperor Spice"));
    muted(T(L"Emperor: Battle for Dune on modern PCs - widescreen, modern graphics and many fixes. "
            L"Pick your settings on the left, then press Play.",
            L"Emperor: Schlacht um Dune auf modernen PCs - Breitbild, moderne Grafik und viele Verbesserungen. "
            L"Links w\u00e4hlst du die Einstellungen, dann auf \u201eSpielen\u201c."),
          X, 140, kContentW, 40);
    heading(T(L"Language", L"Sprache"), 196);
    label(L"Sprache / Language", X, 230, 150, 22);
    languageCombo = combo(X2, 226, 220, 120);
    SendMessageW(languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(languageCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Deutsch"));
    SendMessageW(languageCombo, CB_SETCURSEL, gLang == Lang::DE ? 1 : 0, 0);
    heading(T(L"Shortcut", L"Verkn\u00fcpfung"), 280);
    shortcutButton = button(T(L"Create desktop shortcut", L"Desktop-Verkn\u00fcpfung erstellen"), X, 312, 260);
    heading(T(L"Problems?", L"Probleme?"), 366);
    muted(T(L"See \"Fixes\" for mouse problems. If the game does not start or crashes, tick \"Debug log\" under \"Log\", "
            L"try again and attach data\\emperor.txt to an issue on GitHub.",
            L"Bei Problemen mit der Maus hilft \u201eProbleml\u00f6sung\u201c. Startet das Spiel nicht oder st\u00fcrzt ab: unter \u201eProtokoll\u201c "
            L"das Debug-Log einschalten, nochmal versuchen und data\\emperor.txt bei GitHub anh\u00e4ngen."),
          X, 396, kContentW, 56);
  }

  // --- Graphics ----------------------------------------------------------------------------------
  page = PageGraphics;
  {
    title(T(L"Graphics", L"Grafik"));
    heading(T(L"Picture", L"Bild"), 144);
    fullscreenCheckbox = check(T(L"Fullscreen", L"Vollbild"), X, 174, 300);
    label(T(L"Resolution", L"Aufl\u00f6sung"), X, 208, 150, 22);
    // The height here also controls how tall the drop-down list is, so make it big enough for all entries.
    resolutionCombo = combo(X2, 204, 260, 340);
    for (const Resolution& res : resolutions)
      SendMessageW(resolutionCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(res.label, res.labelDe)));
    // Graphics pack (dgVoodoo + ReShade): off, "Modern" or "HD". Greyed out without the graphics folder.
    label(T(L"Graphics look", L"Grafikstil"), X, 242, 150, 22);
    graphicsCombo = combo(X2, 238, 260, 120);
    SendMessageW(graphicsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"Original (no graphics pack)", L"Original (ohne Grafik-Paket)")));
    SendMessageW(graphicsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"Modern (sharper, smooth edges)", L"Modern (sch\u00e4rfer, glatte Kanten)")));
    SendMessageW(graphicsCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"HD (light and shadows)", L"HD (Licht und Schatten)")));
    if (!graphicsPack::available(std::filesystem::path(getFolderThisExeIsIn()) / L"graphics"))
      EnableWindow(graphicsCombo, FALSE);
    label(T(L"Camera zoom", L"Kamera-Zoom"), X, 276, 150, 22);
    cameraZoomCombo = combo(X2, 272, 260, 120);
    SendMessageW(cameraZoomCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"Original", L"Original")));
    SendMessageW(cameraZoomCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"A little farther (1.25x)", L"Etwas weiter (1,25x)")));
    SendMessageW(cameraZoomCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"Farther (1.5x, recommended)", L"Weiter (1,5x, empfohlen)")));
    SendMessageW(cameraZoomCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"Far (1.75x)", L"Sehr weit (1,75x)")));

    heading(T(L"Motion", L"Bewegung"), 320);
    smoothScrollCheckbox = check(T(L"Smooth map scrolling", L"Fl\u00fcssiges Karten-Scrollen"), X, 350, 420);
    cutscene43Checkbox = check(T(L"Cutscene movies in 4:3 (black bars)", L"Zwischensequenzen in 4:3 (schwarze R\u00e4nder)"), X, 378, 420);

    // Grafik-Werkstatt: AI-upscaled textures, made on this PC from the player's own game files.
    // Greyed out when the launcher was built without the bundled Real-ESRGAN (developer builds).
    heading(T(L"HD textures (AI)", L"HD-Texturen (KI)"), 422);
    hdTexturesCheckbox = check(T(L"Use HD textures", L"HD-Texturen verwenden"), X, 454, 260);
    hdTexturesButton = button(T(L"Generate\u2026", L"Erzeugen\u2026"), X2 + 110, 450, 150);
    hdTexturesStatus = muted(L"", descX, 482, descW, 20);
    if (!hdTextures::toolAvailable(hdToolDir()))
    {
      EnableWindow(hdTexturesCheckbox, FALSE);
      EnableWindow(hdTexturesButton, FALSE);
    }
  }

  // --- Multiplayer -------------------------------------------------------------------------------
  page = PageMultiplayer;
  {
    title(T(L"Multiplayer", L"Mehrspieler"));
    hostGameRadio = check(T(L"Host game / single player", L"Spiel hosten / Einzelspieler"), X, 148, 400, ui::kRadio);
    portsLabel = muted(T(L"The host must open port 4005 (UDP and TCP) in the router.",
                         L"Wer hostet, muss im Router Port 4005 (UDP und TCP) freigeben."), descX, 172, descW, 20);
    connectToServerRadio = check(T(L"Connect to a server", L"Mit Server verbinden"), X, 206, 400, ui::kRadio);
    serverAddressLabel = label(T(L"Server IP or hostname (dynamic DNS works)", L"Server-IP oder Hostname (auch dynamisches DNS)"), descX, 236, descW, 20);
    serverAddressTextbox = put(CreateWindowEx(WS_EX_CLIENTEDGE, WC_EDIT, L"host IP or hostname", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                              S(descX), S(260), S(300), S(26), window, nullptr, nullptr, nullptr));
    gThemedNative.push_back(serverAddressTextbox);
    testNetworkButton = button(T(L"Test connection", L"Verbindung testen"), descX + 312, 258, 180);

    heading(T(L"Must be the same for all players", L"Muss bei allen Spielern gleich sein"), 320);
    mpSettingsLabel = label(L"", descX, 352, descW, 66);
    muted(T(L"If these differ, the games drift apart. Graphics, zoom, fonts and HD textures are up to each player.",
            L"Weichen sie ab, laufen die Spiele auseinander. Grafik, Zoom, Schrift und HD-Texturen darf jeder selbst w\u00e4hlen."),
          descX, 424, descW, 40);
    muted(T(L"The money cheat is never active in multiplayer.", L"Der Geld-Cheat ist im Mehrspieler nie aktiv."), descX, 466, descW, 20);
  }

  // --- Mods & extras -----------------------------------------------------------------------------
  page = PageMods;
  {
    title(T(L"Mods & extras", L"Mods & Extras"));
    heading(L"Mods", 144);
    modsCheckbox = check(T(L"Load mods from the \"mods\" folder", L"Mods aus dem Ordner \u201emods\u201c laden"), X, 174, 420);
    muted(T(L"In multiplayer all players need the same mods.", L"Im Mehrspieler brauchen alle dieselben Mods."), descX, 198, descW, 20);
    modsFolderButton = button(T(L"Open mods folder", L"Mod-Ordner \u00f6ffnen"), descX, 224, 200);
    heading(L"Extras", 278);
    // Greyed out when the launcher was built without the bundled fonts folder (developer builds).
    largeFontCheckbox = check(T(L"Large in-game messages and hints (2x)", L"Gro\u00dfe Meldungen und Hinweise im Spiel (2x)"), X, 308, 460);
    if (!fontPack::available(std::filesystem::path(getFolderThisExeIsIn()) / L"fonts"))
      EnableWindow(largeFontCheckbox, FALSE);
    moneyCheatCheckbox = check(T(L"Money cheat in single player", L"Geld-Cheat im Einzelspieler"), X, 340, 460);
    muted(T(L"Ctrl+Shift+M gives +25,000 credits. Never active in multiplayer.",
            L"Strg+Umschalt+M gibt +25.000 Credits. Im Mehrspieler nie aktiv."), descX, 364, descW, 20);
    groupMoveCheckbox = check(T(L"Group movement: keep formation, arrive together (test)",
                                L"Gruppenbewegung: Formation halten, gemeinsam ankommen (Test)"), X, 400, 560);
    muted(T(L"Ctrl+Shift+F switches it in single player. In multiplayer all players need the same setting.",
            L"Strg+Umschalt+F schaltet im Einzelspieler um. Im Mehrspieler brauchen alle dieselbe Einstellung."), descX, 424, descW, 20);
    radarCalmCheckbox = check(T(L"Calmer radar (no fog flicker at cliffs)", L"Ruhigeres Radar (kein Nebel-Flackern an Klippen)"), X, 460, 560);
  }

  // --- Fixes (workarounds for driver and mouse problems) ----------------------------------------
  page = PageCompat;
  {
    title(T(L"Fixes", L"Probleml\u00f6sung"));
    muted(T(L"Only needed if something does not work - leave them off otherwise.",
            L"Nur n\u00f6tig, wenn etwas nicht klappt - sonst ausgeschaltet lassen."), X, 140, kContentW, 20);
    forceCursorVisibleCheckbox = check(T(L"Always show the mouse pointer", L"Mauszeiger immer zeigen"), X, 178, 420);
    muted(T(L"Use this if there is no mouse pointer in the game.",
            L"Hilft, wenn im Spiel kein Mauszeiger zu sehen ist."), descX, 202, descW, 20);
    disableCursorCaptureCheckbox = check(T(L"Don't keep the mouse in the game window", L"Maus nicht im Spielfenster festhalten"), X, 238, 420);
    muted(T(L"For window mode or a second screen. Scrolling at the screen edge then works less well.",
            L"F\u00fcr den Fenstermodus oder einen zweiten Bildschirm. Das Scrollen am Bildschirmrand klappt dann schlechter."),
          descX, 262, descW, 20);
  }

  // --- Log -----------------------------------------------------------------------------------------
  page = PageLog;
  {
    title(T(L"Log", L"Protokoll"));
    // Debug log: data\emperor.txt + launcher.txt with everything we record (EmperorHooks/DebugLog.cpp,
    // PerfLog.cpp, TextureTrace.cpp, LauncherLog.hpp). Off = nothing is recorded, old files are deleted.
    debugLogCheckbox = check(T(L"Debug log", L"Debug-Log"), X, 148, 420);
    muted(T(L"Records system info, crashes, hangs, stutters and memory in data\\emperor.txt and launcher.txt. "
            L"Off = nothing is recorded.",
            L"Zeichnet Systeminfos, Abst\u00fcrze, H\u00e4nger, Ruckler und Speicher in data\\emperor.txt und launcher.txt auf. "
            L"Aus = es wird nichts aufgezeichnet."),
          descX, 172, descW, 40);
    muted(T(L"Ctrl+Shift+L in the game sets a marker in the log.", L"Strg+Umschalt+L im Spiel setzt eine Marke ins Log."), descX, 214, descW, 20);
    pauseOnStartupCheckbox = check(T(L"Pause on startup (for developers)", L"Beim Start pausieren (f\u00fcr Entwickler)"), X, 258, 420);
    muted(T(L"Stops before the game starts, so a debugger can be attached.",
            L"H\u00e4lt vor dem Spielstart an, damit man einen Debugger anh\u00e4ngen kann."), descX, 282, descW, 20);
  }

  // --- navigation, bottom bar, credits ----------------------------------------------------------
  const wchar_t* navText[PageCount] = { T(L"Start", L"Start"), T(L"Graphics", L"Grafik"), T(L"Multiplayer", L"Mehrspieler"),
                                        T(L"Mods & extras", L"Mods & Extras"), T(L"Fixes", L"Probleml\u00f6sung"), T(L"Log", L"Protokoll") };
  for (int i = 0; i < PageCount; i++)
  {
    gNavButtons[i] = CreateWindowEx(0, WC_BUTTON, navText[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    S(12), S(kHeaderH + 16 + i * 44), S(kSidebarW - 24), S(38), window, nullptr, nullptr, nullptr);
    ui::trackHover(gNavButtons[i]);
  }
  versionLabel = CreateWindowEx(0, WC_STATIC, kLauncherVersion, WS_CHILD | WS_VISIBLE, S(20), S(kClientH - 74), S(kSidebarW - 28), S(20), window, nullptr, nullptr, nullptr);
  githubLink = CreateWindowEx(0, WC_STATIC, T(L"Based on Emperor Reborn", L"Basiert auf Emperor Reborn"), WS_CHILD | WS_VISIBLE | SS_NOTIFY, S(20), S(kClientH - 52), S(kSidebarW - 28), S(20), window, nullptr, nullptr, nullptr);
  facebookLink = CreateWindowEx(0, WC_STATIC, T(L"Emperor Spice on GitHub", L"Emperor Spice auf GitHub"), WS_CHILD | WS_VISIBLE | SS_NOTIFY, S(20), S(kClientH - 32), S(kSidebarW - 28), S(20), window, nullptr, nullptr, nullptr);
  gSidebarLabels = { versionLabel, githubLink, facebookLink };

  statusLabel = CreateWindowEx(0, WC_STATIC, L"", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, S(kContentX), S(kBottomBarY + 26), S(kContentW - 200), S(20), window, nullptr, nullptr, nullptr);
  gMutedLabels.push_back(statusLabel);
  playButton = CreateWindowEx(0, WC_BUTTON, T(L"Play", L"Spielen"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                              S(kClientW - 16 - 180), S(kBottomBarY + 14), S(180), S(42), window, nullptr, nullptr, nullptr);
  ui::trackHover(playButton);

  // Fonts: Segoe UI everywhere (titles and headings were set above, keep them).
  for (HWND c = GetWindow(window, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
  {
    HFONT f = reinterpret_cast<HFONT>(SendMessageW(c, WM_GETFONT, 0, 0));
    if (f != gTitleFont && f != gHeadingFont)
      SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), FALSE);
  }
  SendMessageW(playButton, WM_SETFONT, reinterpret_cast<WPARAM>(gPlayFont), FALSE);
  SendMessageW(versionLabel, WM_SETFONT, reinterpret_cast<WPARAM>(gBoldFont), FALSE);
  SendMessageW(githubLink, WM_SETFONT, reinterpret_cast<WPARAM>(gLinkFont), FALSE);
  SendMessageW(facebookLink, WM_SETFONT, reinterpret_cast<WPARAM>(gLinkFont), FALSE);
  for (HWND n : gNavButtons)
    SendMessageW(n, WM_SETFONT, reinterpret_cast<WPARAM>(gBoldFont), FALSE);

  // Size the window so its client area fits the layout, centred on the screen.
  RECT wr = { 0, 0, S(kClientW), S(kClientH) };
  AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
  int winW = wr.right - wr.left, winH = wr.bottom - wr.top;
  SetWindowPos(window, nullptr, (GetSystemMetrics(SM_CXSCREEN) - winW) / 2, (GetSystemMetrics(SM_CYSCREEN) - winH) / 2, winW, winH, SWP_NOZORDER);

  showPage(PageStart);

  defWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));

  loadAndApplySettings();
  applyTheme();
  refreshStatus();

  ShowWindow(window, SW_SHOW);

  MSG message = { 0 };
  while (GetMessage(&message, nullptr, 0, 0))
  {
    if (window && IsDialogMessage(window, &message)) // Tab / arrow keys between the controls
      continue;
    TranslateMessage(&message);
    DispatchMessage(&message);
  }

  if (playClicked)
  {
    int gameWidth = settings.screenWidth;
    int gameHeight = settings.screenHeight;

    if (gameWidth <= 0 || gameHeight <= 0)
    {
      // "Desktop (match screen)" was selected - match the monitor like the original behaviour.
      gameHeight = getMainMonitorHeight();

      if (!settings.fullscreen)
        gameHeight = std::max(int(gameHeight * 0.8), 600);

      // Lots of stuff in the game assumes a 4:3 screen ratio.
      gameWidth = int(float(gameHeight) * 4.0f / 3.0f);
    }

    // The fixed presets are all 4:3, which the game expects. Picking a low one avoids locking to
    // the (often very high) desktop resolution, which makes the in-game fonts unreadably small.
    writeGraphicsSettings(gameWidth, gameHeight);
    launcherLog::write("Play: game resolution %dx%d, fullscreen %d, graphics pack %d (style %d), large font %d, mods %d, HD textures %d",
                       gameWidth, gameHeight, int(settings.fullscreen), int(settings.graphicsPack), settings.graphicsStyle,
                       int(settings.largeFont), int(settings.mods), int(settings.hdTextures));

    // Install or remove the graphics pack (dgVoodoo2 + ReShade) in the game folder.
    std::wstring packError = graphicsPack::apply(std::filesystem::path(getFolderThisExeIsIn()) / L"graphics",
                                                 std::filesystem::path(installDirUnicode), settings.graphicsPack,
                                                 settings.graphicsStyle == 1);
    launcherLog::write("Graphics pack: %s", packError.empty() ? "ok" : launcherLog::utf8(packError).c_str());
    if (!packError.empty())
      MessageBoxW(nullptr, (T(L"The graphics pack could not be set up - the game starts without it.\n\n",
                              L"Das Grafik-Paket konnte nicht eingerichtet werden - das Spiel startet ohne.\n\n") + packError).c_str(),
                  L"Emperor Spice", MB_OK | MB_ICONWARNING);

    // Large in-game text: make sure the HD font archives are in the DATA folder (EmperorHooks switches
    // the fonts only when it finds them, so a failure here just means the original fonts).
    if (settings.largeFont)
    {
      std::wstring fontError = fontPack::install(std::filesystem::path(getFolderThisExeIsIn()) / L"fonts",
                                                 std::filesystem::path(installDirUnicode) / L"DATA");
      launcherLog::write("Large fonts: %s", fontError.empty() ? "ok" : launcherLog::utf8(fontError).c_str());
      if (!fontError.empty())
        MessageBoxW(nullptr, (T(L"The large fonts could not be installed - the game starts with the normal text size.\n\n",
                                L"Die gro\u00dfe Schrift konnte nicht eingerichtet werden - das Spiel startet mit normaler Schrift.\n\n") + fontError).c_str(),
                    L"Emperor Spice", MB_OK | MB_ICONWARNING);
    }

    // Mods: rebuild the archives from the "mods" folder (or only remove the old ones when switched off).
    {
      modPack::Result modResult = modPack::apply(std::filesystem::path(getFolderThisExeIsIn()) / L"mods",
                                                 std::filesystem::path(installDirUnicode) / L"DATA", settings.mods);
      launcherLog::write("Mods: %d files, %s%s%s", modResult.fileCount,
                         modResult.error.empty() ? "ok" : launcherLog::utf8(modResult.error).c_str(),
                         modResult.warnings.empty() ? "" : ", skipped: ", launcherLog::utf8(modResult.warnings).c_str());
      if (!modResult.error.empty())
        MessageBoxW(nullptr, (T(L"The mods could not be set up - the game may start without them.\n\n",
                                L"Die Mods konnten nicht eingerichtet werden - das Spiel startet eventuell ohne.\n\n") + modResult.error).c_str(),
                    L"Emperor Spice", MB_OK | MB_ICONWARNING);
      else if (!modResult.warnings.empty())
        MessageBoxW(nullptr, (T(L"These mod folders were skipped because the game has no archive with that name:\n\n",
                                L"Diese Mod-Ordner wurden \u00fcbersprungen, weil das Spiel kein Archiv mit diesem Namen hat:\n\n") + modResult.warnings).c_str(),
                    L"Emperor Spice", MB_OK | MB_ICONINFORMATION);
    }

    // HD textures (Grafik-Werkstatt): put the generated archive into DATA, or take it out. Last, so its
    // archive number comes after the mods; mod files still win because they are newer.
    {
      std::wstring hdError = hdTextures::apply(std::filesystem::path(installDirUnicode), settings.hdTextures);
      launcherLog::write("HD textures %s: %s", settings.hdTextures ? "on" : "off", hdError.empty() ? "ok" : launcherLog::utf8(hdError).c_str());
      if (!hdError.empty())
        MessageBoxW(nullptr, (T(L"The HD textures could not be switched - the game may start with the original textures.\n\n",
                                L"Die HD-Texturen konnten nicht umgeschaltet werden - das Spiel startet eventuell mit den Original-Texturen.\n\n") + hdError).c_str(),
                    L"Emperor Spice", MB_OK | MB_ICONWARNING);
    }

    createGlobalCommsFileMappingHandle();

    launcherLog::write("Starting Game.exe");
    PROCESS_INFORMATION runGameData = runGameExe();

    sendEmperorDatDataToGameProcessViaAnonymousMapping(runGameData.hProcess, runGameData.dwThreadId);

    DWORD ExitCode = 0;
    waitForExit(&runGameData, &ExitCode);
    launcherLog::write("Game.exe ended, exit code 0x%08lX", ExitCode);

    closeGlobalCommsHandle();

    return ExitCode;
  }

  return 0;
}
