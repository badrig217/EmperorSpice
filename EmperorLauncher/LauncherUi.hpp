#pragma once
// Launcher look: light / dark theme and the self-drawn controls that follow it.
//
// The standard Windows check boxes and radio buttons can't be coloured (their text stays black in a dark
// window), so the launcher uses its own "SpiceCheck" control. It understands the usual button messages
// (BM_GETCHECK, BM_SETCHECK) and tells its parent WM_COMMAND/BN_CLICKED, so the rest of the launcher
// treats it like a normal check box. Push buttons and the navigation are owner-drawn (BS_OWNERDRAW,
// painted by drawButton from the parent's WM_DRAWITEM). Combo boxes and edit fields stay native and
// get Windows' own dark theme ("DarkMode_CFD") plus our colours via WM_CTLCOLOR*.
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace ui
{
  struct Palette
  {
    COLORREF window, sidebar, header, text, muted, accent, accentText, border, control, controlHover, selected, link;
  };

  // Spice orange as the accent, warm greys around it (matches the header banner).
  inline const Palette kLight = { RGB(250, 249, 247), RGB(240, 237, 232), RGB(24, 16, 11), RGB(32, 30, 28), RGB(110, 104, 98),
                                  RGB(201, 112, 32), RGB(255, 255, 255), RGB(208, 202, 194), RGB(255, 255, 255),
                                  RGB(244, 240, 234), RGB(232, 222, 208), RGB(176, 88, 18) };
  inline const Palette kDark = { RGB(30, 28, 27), RGB(23, 21, 20), RGB(18, 12, 9), RGB(236, 232, 226), RGB(160, 152, 144),
                                 RGB(222, 138, 52), RGB(28, 18, 10), RGB(70, 64, 60), RGB(42, 39, 37),
                                 RGB(52, 48, 45), RGB(64, 52, 40), RGB(240, 160, 80) };

  inline bool gDark = false;
  inline const Palette& pal() { return gDark ? kDark : kLight; }

  inline UINT* dpiPtr() { static UINT d = 96; return &d; }
  inline int px(int v) { return MulDiv(v, int(*dpiPtr()), 96); }

  // Windows' own app theme ("Choose your app mode"): true = dark.
  inline bool windowsUsesDarkApps()
  {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
      return false;
    return value == 0;
  }

  // Brushes for the current palette (recreated when the theme changes).
  struct Brushes
  {
    HBRUSH window = nullptr, sidebar = nullptr, control = nullptr;
  };
  inline Brushes& brushes() { static Brushes b; return b; }
  inline void rebuildBrushes()
  {
    Brushes& b = brushes();
    for (HBRUSH* h : { &b.window, &b.sidebar, &b.control })
      if (*h) DeleteObject(*h);
    b.window = CreateSolidBrush(pal().window);
    b.sidebar = CreateSolidBrush(pal().sidebar);
    b.control = CreateSolidBrush(pal().control);
  }

  inline void fillRect(HDC dc, const RECT& r, COLORREF c)
  {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
  }

  inline void roundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF line)
  {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
  }

  // Title bar in the theme's colour (Windows 10 2004+/11; older systems simply ignore it).
  inline void themeTitleBar(HWND w)
  {
    BOOL dark = gDark;
    DwmSetWindowAttribute(w, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    DwmSetWindowAttribute(w, 19 /*pre-20H1 value*/, &dark, sizeof(dark));
  }

  // Native controls that can follow the theme by themselves.
  inline void themeNative(HWND control)
  {
    SetWindowTheme(control, gDark ? L"DarkMode_CFD" : nullptr, nullptr);
  }

  // ------------------------------------------------------------------ SpiceCheck (check box / radio)
  // Class styles (low word): kRadio = behaves like an auto radio button within its parent.
  const DWORD kRadio = 0x0001;
  inline const wchar_t kCheckClass[] = L"SpiceCheck";

  struct CheckState
  {
    bool checked = false, hot = false, pressed = false;
    HFONT font = nullptr; // custom window classes have to keep WM_SETFONT themselves
  };

  inline CheckState* checkState(HWND h) { return reinterpret_cast<CheckState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

  inline void clickCheck(HWND h)
  {
    CheckState* s = checkState(h);
    if (!s) return;
    if (GetWindowLongW(h, GWL_STYLE) & kRadio)
    {
      // uncheck the other radios of the same parent
      for (HWND c = GetWindow(GetParent(h), GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
      {
        wchar_t cls[32];
        GetClassNameW(c, cls, 32);
        if (c != h && lstrcmpW(cls, kCheckClass) == 0 && (GetWindowLongW(c, GWL_STYLE) & kRadio) && checkState(c) && checkState(c)->checked)
        {
          checkState(c)->checked = false;
          InvalidateRect(c, nullptr, TRUE);
        }
      }
      s->checked = true;
    }
    else
      s->checked = !s->checked;
    InvalidateRect(h, nullptr, TRUE);
    SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), reinterpret_cast<LPARAM>(h));
  }

  inline void paintCheck(HWND h, HDC dc)
  {
    RECT rc;
    GetClientRect(h, &rc);
    // background = whatever the parent paints behind us
    HBRUSH bg = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(h), WM_CTLCOLORBTN, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(h)));
    FillRect(dc, &rc, bg ? bg : brushes().window);

    CheckState* s = checkState(h);
    const bool radio = (GetWindowLongW(h, GWL_STYLE) & kRadio) != 0;
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const bool checked = s && s->checked;
    const Palette& p = pal();
    const int box = px(16);
    RECT b = { rc.left, rc.top + (px(22) - box) / 2, rc.left + box, rc.top + (px(22) - box) / 2 + box };

    COLORREF fill = checked ? p.accent : (s && s->hot ? p.controlHover : p.control);
    COLORREF line = checked ? p.accent : p.border;
    if (!enabled) { fill = checked ? p.border : p.control; line = p.border; }
    if (radio)
    {
      HBRUSH br = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, 1, line);
      HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
      Ellipse(dc, b.left, b.top, b.right, b.bottom);
      SelectObject(dc, ob); SelectObject(dc, op);
      DeleteObject(br); DeleteObject(pen);
      if (checked)
      {
        int d = px(6);
        HBRUSH dot = CreateSolidBrush(p.accentText);
        HPEN np = CreatePen(PS_SOLID, 1, p.accentText);
        ob = SelectObject(dc, dot); op = SelectObject(dc, np);
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        Ellipse(dc, cx - d / 2, cy - d / 2, cx + d / 2 + 1, cy + d / 2 + 1);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(dot); DeleteObject(np);
      }
    }
    else
    {
      roundRect(dc, b, px(5), fill, line);
      if (checked)
      {
        HPEN pen = CreatePen(PS_SOLID, std::max(2, px(2)), p.accentText);
        HGDIOBJ op = SelectObject(dc, pen);
        POINT pts[3] = { { b.left + px(4), b.top + px(8) }, { b.left + px(7), b.top + px(11) }, { b.left + px(12), b.top + px(5) } };
        Polyline(dc, pts, 3);
        SelectObject(dc, op);
        DeleteObject(pen);
      }
    }

    wchar_t text[512];
    GetWindowTextW(h, text, 512);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
    HGDIOBJ of = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? p.text : p.muted);
    RECT tr = { b.right + px(8), rc.top, rc.right, rc.bottom };
    RECT measure = tr;
    DrawTextW(dc, text, -1, &measure, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    const bool oneLine = measure.bottom - measure.top <= px(22) - px(4);
    if (oneLine)
    {
      tr.bottom = rc.top + px(22);
      DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    else
    {
      tr.top += px(3);
      DrawTextW(dc, text, -1, &tr, DT_WORDBREAK | DT_NOPREFIX);
    }
    if (GetFocus() == h && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
    {
      RECT fr = { tr.left - px(3), rc.top + px(1), std::min(rc.right, tr.left + (measure.right - measure.left) + px(3)), rc.top + px(21) };
      DrawFocusRect(dc, &fr);
    }
    SelectObject(dc, of);
  }

  inline LRESULT CALLBACK checkProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
  {
    CheckState* s = checkState(h);
    switch (msg)
    {
    case WM_NCCREATE:
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new CheckState));
      break;
    case WM_NCDESTROY:
      delete s;
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      break;
    case WM_SETFONT:
      if (s) s->font = reinterpret_cast<HFONT>(wp);
      if (LOWORD(lp)) InvalidateRect(h, nullptr, TRUE);
      return 0;
    case WM_GETFONT:
      return reinterpret_cast<LRESULT>(s ? s->font : nullptr);
    case BM_GETCHECK:
      return s && s->checked ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK:
      if (s) { s->checked = wp == BST_CHECKED; InvalidateRect(h, nullptr, TRUE); }
      return 0;
    case WM_GETDLGCODE:
      return DLGC_BUTTON | ((GetWindowLongW(h, GWL_STYLE) & kRadio) ? DLGC_RADIOBUTTON : 0);
    case WM_SETTEXT:
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
    {
      LRESULT r = DefWindowProcW(h, msg, wp, lp);
      InvalidateRect(h, nullptr, TRUE);
      return r;
    }
    case WM_MOUSEMOVE:
      if (s && !s->hot)
      {
        s->hot = true;
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        InvalidateRect(h, nullptr, TRUE);
      }
      return 0;
    case WM_MOUSELEAVE:
      if (s) { s->hot = false; InvalidateRect(h, nullptr, TRUE); }
      return 0;
    case WM_LBUTTONDOWN:
      SetFocus(h);
      SetCapture(h);
      if (s) s->pressed = true;
      return 0;
    case WM_LBUTTONUP:
      if (GetCapture() == h) ReleaseCapture();
      if (s && s->pressed)
      {
        s->pressed = false;
        POINT pt = { short(LOWORD(lp)), short(HIWORD(lp)) };
        RECT rc;
        GetClientRect(h, &rc);
        if (PtInRect(&rc, pt)) clickCheck(h);
      }
      return 0;
    case WM_KEYUP:
      if (wp == VK_SPACE) { clickCheck(h); return 0; }
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      // double buffered: no flicker when the hover state changes
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ ob = SelectObject(mem, bmp);
      paintCheck(h, mem);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, ob);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
  }

  inline void registerControls()
  {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = checkProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = kCheckClass;
    RegisterClassW(&wc);
  }

  // ------------------------------------------------------------------ owner-drawn buttons
  enum class ButtonKind { Normal, Primary, Nav, NavSelected, Toggle };

  inline void drawButton(const DRAWITEMSTRUCT* d, ButtonKind kind, bool hot)
  {
    const Palette& p = pal();
    HDC dc = d->hDC;
    RECT rc = d->rcItem;
    const bool pressed = (d->itemState & ODS_SELECTED) != 0;
    const bool disabled = (d->itemState & ODS_DISABLED) != 0;
    // clear behind rounded corners
    COLORREF behind = (kind == ButtonKind::Nav || kind == ButtonKind::NavSelected) ? p.sidebar : kind == ButtonKind::Toggle ? p.header : p.window;
    fillRect(dc, rc, behind);

    COLORREF fill, line, text;
    switch (kind)
    {
    case ButtonKind::Primary:
      fill = disabled ? p.border : (pressed ? p.link : p.accent);
      line = fill;
      text = p.accentText;
      if (hot && !pressed && !disabled) fill = line = RGB(std::min(255, GetRValue(p.accent) + 16), std::min(255, GetGValue(p.accent) + 12), GetBValue(p.accent));
      break;
    case ButtonKind::Nav:
      fill = hot ? p.controlHover : p.sidebar;
      line = fill;
      text = p.text;
      break;
    case ButtonKind::NavSelected:
      fill = p.selected;
      line = fill;
      text = p.text;
      break;
    case ButtonKind::Toggle:
      fill = hot ? RGB(60, 44, 32) : RGB(40, 30, 22);
      line = RGB(90, 70, 52);
      text = RGB(240, 214, 170);
      break;
    default:
      fill = pressed ? p.selected : (hot ? p.controlHover : p.control);
      line = p.border;
      text = disabled ? p.muted : p.text;
    }
    roundRect(dc, rc, px(8), fill, line);
    if (kind == ButtonKind::NavSelected)
    {
      RECT bar = { rc.left + px(2), rc.top + px(9), rc.left + px(5), rc.bottom - px(9) };
      fillRect(dc, bar, p.accent);
    }

    wchar_t label[256];
    GetWindowTextW(d->hwndItem, label, 256);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    RECT tr = rc;
    UINT align = DT_CENTER;
    if (kind == ButtonKind::Nav || kind == ButtonKind::NavSelected)
    {
      tr.left += px(16);
      align = DT_LEFT;
    }
    DrawTextW(dc, label, -1, &tr, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT))
    {
      RECT fr = rc;
      InflateRect(&fr, -px(3), -px(3));
      DrawFocusRect(dc, &fr);
    }
  }

  // Hover tracking for owner-drawn buttons: subclass, remember which one the mouse is over.
  inline HWND& hotButton() { static HWND h = nullptr; return h; }
  inline LRESULT CALLBACK hoverProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
  {
    if (msg == WM_MOUSEMOVE && hotButton() != h)
    {
      HWND old = hotButton();
      hotButton() = h;
      if (old) InvalidateRect(old, nullptr, FALSE);
      InvalidateRect(h, nullptr, FALSE);
      TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
      TrackMouseEvent(&t);
    }
    else if (msg == WM_MOUSELEAVE && hotButton() == h)
    {
      hotButton() = nullptr;
      InvalidateRect(h, nullptr, FALSE);
    }
    else if (msg == WM_LBUTTONDBLCLK)
      msg = WM_LBUTTONDOWN; // fast clicks on owner-drawn buttons arrive as double clicks
    else if (msg == WM_SETCURSOR)
    {
      SetCursor(LoadCursor(nullptr, IDC_HAND));
      return TRUE;
    }
    return DefSubclassProc(h, msg, wp, lp);
  }
  inline void trackHover(HWND button) { SetWindowSubclass(button, hoverProc, 1, 0); }
}
