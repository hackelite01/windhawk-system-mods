// ==WindhawkMod==
// @id              file-operation-glass
// @name            File Operation Glass
// @description     Modern copy/move/delete dialog: dark glass window, progress ring, live speed graph, ETA and Pause/Cancel
// @version         0.1.0
// @author          hackelite01
// @github          https://github.com/hackelite01
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -luuid -lgdi32 -luser32 -ldwmapi
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# File Operation Glass

Restyles the Windows file operation dialog (copy, move, delete, recycle).

- Dark window with an acrylic or Mica backdrop, rounded corners and an accent border.
- A panel drawn over the dialog with a progress ring, live transfer speed,
  a speed graph, time remaining, items remaining and the current file.
- Pause and Cancel buttons that press the real buttons of the dialog.
- Theme presets (Glass, Midnight, Aurora, Light) and a custom accent color.

Windows still does all of the copying, so conflicts, errors and cancelling
behave exactly as before. If anything goes wrong, disable the mod and the
original dialog is back.

The idea of a restyled file operation window comes from
[File Operation Styler](https://github.com/digart11/File-Operation-Styler) by
digart11. This mod is a separate implementation that reads progress through
UI Automation and draws its own panel.

## Notes
- Applies to new dialogs only; a dialog that is already open keeps its look.
- Reading speed and time uses the dialog's own text, which is parsed for
  English number and size formats. The progress ring uses the percentage in
  the window title.
- Tested on Windows 11 only.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- theme: glass
  $name: Theme
  $options:
    - glass: Glass (dark blue)
    - midnight: Midnight (violet)
    - aurora: Aurora (green)
    - light: Light
- accent: ""
  $name: Custom accent color
  $description: Hex color such as #ff6a3d. Leave empty to use the theme color.
- backdrop: acrylic
  $name: Window backdrop
  $options:
    - acrylic: Acrylic
    - mica: Mica
    - none: Default
- opacity: 245
  $name: Panel opacity
  $description: 200 to 255. Lower values let the original dialog show through.
- showGraph: true
  $name: Show speed graph
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const wchar_t* kDialogClass = L"OperationStatusWindow";
constexpr const wchar_t* kOverlayClass = L"HackeliteFileOpGlassOverlay";
constexpr UINT kTickMs = 120;

// ---------------------------------------------------------------- settings

struct Theme {
    COLORREF bg, card, text, sub, accent, track;
};

struct Settings {
    std::wstring theme = L"glass";
    std::wstring accent;
    std::wstring backdrop = L"acrylic";
    int opacity = 245;
    bool showGraph = true;
};

Settings g_settings;

std::wstring ReadString(const wchar_t* name) {
    PCWSTR v = Wh_GetStringSetting(name);
    std::wstring s = v ? v : L"";
    Wh_FreeStringSetting(v);
    return s;
}

void LoadSettings() {
    Settings s;
    s.theme = ReadString(L"theme");
    s.accent = ReadString(L"accent");
    s.backdrop = ReadString(L"backdrop");
    s.opacity = std::clamp(Wh_GetIntSetting(L"opacity"), 200, 255);
    s.showGraph = Wh_GetIntSetting(L"showGraph") != 0;
    g_settings = s;
}

bool ParseHexColor(const std::wstring& in, COLORREF* out) {
    std::wstring s = in;
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6) return false;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (end != s.c_str() + 6) return false;
    *out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

Theme GetTheme() {
    Theme t;
    if (g_settings.theme == L"midnight") {
        t = {RGB(14, 12, 24), RGB(28, 24, 46), RGB(255, 255, 255),
             RGB(170, 160, 195), RGB(160, 120, 255), RGB(52, 46, 80)};
    } else if (g_settings.theme == L"aurora") {
        t = {RGB(10, 26, 26), RGB(18, 44, 44), RGB(255, 255, 255),
             RGB(150, 190, 184), RGB(60, 220, 170), RGB(32, 66, 64)};
    } else if (g_settings.theme == L"light") {
        t = {RGB(244, 246, 250), RGB(255, 255, 255), RGB(24, 28, 36),
             RGB(100, 108, 120), RGB(10, 100, 220), RGB(220, 224, 232)};
    } else {
        t = {RGB(18, 24, 36), RGB(30, 40, 58), RGB(255, 255, 255),
             RGB(160, 172, 190), RGB(77, 163, 255), RGB(52, 64, 86)};
    }
    COLORREF custom;
    if (ParseHexColor(g_settings.accent, &custom)) t.accent = custom;
    return t;
}

COLORREF Blend(COLORREF a, COLORREF b, int pctB) {
    auto mix = [&](int x, int y) { return x + (y - x) * pctB / 100; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
               mix(GetBValue(a), GetBValue(b)));
}

// ------------------------------------------------------------------ state

struct Info {
    std::wstring status, file, eta, items;
    int percent = -1;
    bool paused = false;
};

struct Panel {
    HWND dialog = nullptr;
    HWND overlay = nullptr;
    Info info;
    bool styled = false;
    double lastRemaining = -1;
    DWORD lastSample = 0;
    double speed = 0;  // bytes per second, smoothed
    std::vector<float> history;
    IUIAutomationElement* pauseEl = nullptr;
    IUIAutomationElement* cancelEl = nullptr;
};

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<DWORD> g_threadId{0};

IUIAutomation* g_uia = nullptr;
std::map<HWND, Panel*> g_panels;
HFONT g_fTitle = nullptr, g_fBig = nullptr, g_fSmall = nullptr, g_fPct = nullptr,
      g_fBtn = nullptr;
double g_scale = 1.0;
int g_scanCounter = 0;

int Px(double v) { return static_cast<int>(v * g_scale + 0.5); }

HFONT MakeFont(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable");
}

// ------------------------------------------------------------ text parsing

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// "12 (3.4 MB)" -> bytes inside the parentheses, or -1.
double ParseBytesInParens(const std::wstring& s) {
    size_t a = s.find(L'(');
    size_t b = (a == std::wstring::npos) ? a : s.find(L')', a);
    if (a == std::wstring::npos || b == std::wstring::npos) return -1;
    std::wstring t = s.substr(a + 1, b - a - 1);
    for (auto& c : t) {
        if (c == L',') c = L'.';
    }
    wchar_t* end = nullptr;
    double v = wcstod(t.c_str(), &end);
    if (end == t.c_str()) return -1;
    while (*end == L' ') end++;
    std::wstring unit = Lower(end);
    double mul = 1;
    if (unit.rfind(L"kb", 0) == 0) mul = 1024.0;
    else if (unit.rfind(L"mb", 0) == 0) mul = 1024.0 * 1024;
    else if (unit.rfind(L"gb", 0) == 0) mul = 1024.0 * 1024 * 1024;
    else if (unit.rfind(L"tb", 0) == 0) mul = 1024.0 * 1024 * 1024 * 1024;
    return v * mul;
}

std::wstring FormatSpeed(double bps) {
    wchar_t buf[64];
    if (bps >= 1024.0 * 1024 * 1024) {
        swprintf(buf, 64, L"%.2f GB/s", bps / (1024.0 * 1024 * 1024));
    } else if (bps >= 1024.0 * 1024) {
        swprintf(buf, 64, L"%.1f MB/s", bps / (1024.0 * 1024));
    } else if (bps >= 1024.0) {
        swprintf(buf, 64, L"%.0f KB/s", bps / 1024.0);
    } else {
        swprintf(buf, 64, L"%.0f B/s", bps);
    }
    return buf;
}

// ------------------------------------------------------------ UI Automation

bool PressElement(IUIAutomationElement* el) {
    if (!el) return false;
    IUIAutomationInvokePattern* inv = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId,
                                          IID_IUIAutomationInvokePattern,
                                          reinterpret_cast<void**>(&inv))) &&
        inv) {
        HRESULT hr = inv->Invoke();
        inv->Release();
        if (SUCCEEDED(hr)) return true;
    }
    IUIAutomationTogglePattern* tg = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TogglePatternId,
                                          IID_IUIAutomationTogglePattern,
                                          reinterpret_cast<void**>(&tg))) &&
        tg) {
        HRESULT hr = tg->Toggle();
        tg->Release();
        return SUCCEEDED(hr);
    }
    return false;
}

std::wstring TakeBstr(BSTR b) {
    std::wstring s = b ? b : L"";
    SysFreeString(b);
    return s;
}

void Scan(Panel* p) {
    if (!g_uia) return;
    IUIAutomationElement* root = nullptr;
    if (FAILED(g_uia->ElementFromHandle(p->dialog, &root)) || !root) return;
    IUIAutomationCondition* all = nullptr;
    g_uia->CreateTrueCondition(&all);
    IUIAutomationElementArray* arr = nullptr;
    if (all) root->FindAll(TreeScope_Descendants, all, &arr);

    if (arr) {
        if (p->pauseEl) { p->pauseEl->Release(); p->pauseEl = nullptr; }
        if (p->cancelEl) { p->cancelEl->Release(); p->cancelEl = nullptr; }

        int count = 0;
        arr->get_Length(&count);
        for (int i = 0; i < count; i++) {
            IUIAutomationElement* el = nullptr;
            if (FAILED(arr->GetElement(i, &el)) || !el) continue;
            BSTR idb = nullptr;
            el->get_CurrentAutomationId(&idb);
            std::wstring id = TakeBstr(idb);
            bool keep = false;
            if (id == L"eltPauseButton") {
                BSTR nb = nullptr;
                el->get_CurrentName(&nb);
                p->info.paused = Lower(TakeBstr(nb)).find(L"resume") !=
                                 std::wstring::npos;
                p->pauseEl = el;
                keep = true;
            } else if (id == L"eltCancelButton") {
                p->cancelEl = el;
                keep = true;
            } else if (id == L"eltStartText" || id == L"eltItemName" ||
                       id == L"eltTimeRemaining" || id == L"eltItemsRemaining") {
                BSTR nb = nullptr;
                el->get_CurrentName(&nb);
                std::wstring name = TakeBstr(nb);
                if (id == L"eltStartText") p->info.status = name;
                else if (id == L"eltItemName") p->info.file = name;
                else if (id == L"eltTimeRemaining") p->info.eta = name;
                else p->info.items = name;
            }
            if (!keep) el->Release();
        }
        arr->Release();
    }
    if (all) all->Release();
    root->Release();

    // Percentage from the window title ("74% complete").
    wchar_t title[256] = L"";
    GetWindowTextW(p->dialog, title, 256);
    p->info.percent = iswdigit(title[0]) ? static_cast<int>(wcstol(title, nullptr, 10))
                                         : -1;

    // Speed from the change in remaining bytes.
    double remaining = ParseBytesInParens(p->info.items);
    DWORD now = GetTickCount();
    if (remaining >= 0 && p->lastRemaining >= 0 && now > p->lastSample) {
        double dt = (now - p->lastSample) / 1000.0;
        double inst = (p->lastRemaining - remaining) / dt;
        if (inst < 0) inst = 0;
        p->speed = p->speed <= 0 ? inst : p->speed * 0.7 + inst * 0.3;
    }
    if (remaining >= 0) {
        p->lastRemaining = remaining;
        p->lastSample = now;
    }
    if (p->info.paused) p->speed = 0;
}

// --------------------------------------------------------------------- DWM

void StyleDialog(Panel* p, const Theme& t) {
    BOOL dark = (g_settings.theme != L"light");
    DwmSetWindowAttribute(p->dialog, 20 /*USE_IMMERSIVE_DARK_MODE*/, &dark,
                          sizeof(dark));
    int backdrop = g_settings.backdrop == L"mica"     ? 2
                   : g_settings.backdrop == L"acrylic" ? 3
                                                       : 0;
    if (backdrop) {
        DwmSetWindowAttribute(p->dialog, 38 /*SYSTEMBACKDROP_TYPE*/, &backdrop,
                              sizeof(backdrop));
    }
    int corner = 2;  // round
    DwmSetWindowAttribute(p->dialog, 33 /*WINDOW_CORNER_PREFERENCE*/, &corner,
                          sizeof(corner));
    COLORREF border = t.accent;
    DwmSetWindowAttribute(p->dialog, 34 /*BORDER_COLOR*/, &border, sizeof(border));
    COLORREF caption = t.bg;
    DwmSetWindowAttribute(p->dialog, 35 /*CAPTION_COLOR*/, &caption,
                          sizeof(caption));
    p->styled = true;
}

// ----------------------------------------------------------------- painting

struct Layout {
    RECT ring, graph, btnPause, btnCancel;
    int textLeft, textRight, top;
};

Layout MakeLayout(int w, int h) {
    Layout L{};
    int m = Px(24);
    int ringD = std::min(h - 2 * m - Px(20), static_cast<int>(w * 0.36));
    ringD = std::max(ringD, Px(80));
    L.ring = {m, m + Px(6), m + ringD, m + Px(6) + ringD};
    L.textLeft = m + ringD + Px(28);
    L.textRight = w - m;
    L.top = m;
    int btnH = Px(34), btnW = Px(112);
    L.btnCancel = {w - m - btnW, h - m - btnH, w - m, h - m};
    L.btnPause = {L.btnCancel.left - Px(10) - btnW, h - m - btnH,
                  L.btnCancel.left - Px(10), h - m};
    L.graph = {L.textLeft, L.top + Px(150), L.textRight, h - m - btnH - Px(14)};
    return L;
}

void Text(HDC dc, HFONT f, COLORREF c, const std::wstring& s, RECT r,
          UINT flags = DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), -1, &r, flags);
}

void RoundBox(HDC dc, RECT r, COLORREF fill, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pen);
}

void DrawRing(HDC dc, const RECT& r, int percent, const Theme& t) {
    int thick = Px(12);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int rad = (r.right - r.left) / 2 - thick / 2;
    RECT box{cx - rad, cy - rad, cx + rad, cy + rad};

    LOGBRUSH lb{BS_SOLID, t.track, 0};
    HPEN track = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT, thick, &lb, 0,
                              nullptr);
    HGDIOBJ op = SelectObject(dc, track);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, box.left, box.top, box.right, box.bottom);
    SelectObject(dc, op);
    DeleteObject(track);

    LOGBRUSH lb2{BS_SOLID, t.accent, 0};
    HPEN prog = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND, thick, &lb2,
                             0, nullptr);
    SelectObject(dc, prog);
    SetArcDirection(dc, AD_CLOCKWISE);
    const double pi = 3.14159265358979;
    double startDeg, sweepDeg;
    if (percent >= 0) {
        startDeg = -90;
        sweepDeg = 360.0 * std::clamp(percent, 0, 100) / 100.0;
    } else {  // unknown progress: spinning arc
        startDeg = (GetTickCount() / 4) % 360;
        sweepDeg = 90;
    }
    if (sweepDeg > 0.5) {
        double a0 = startDeg * pi / 180, a1 = (startDeg + sweepDeg) * pi / 180;
        int x0 = cx + static_cast<int>(rad * std::cos(a0));
        int y0 = cy + static_cast<int>(rad * std::sin(a0));
        int x1 = cx + static_cast<int>(rad * std::cos(a1));
        int y1 = cy + static_cast<int>(rad * std::sin(a1));
        if (sweepDeg >= 359.5) {
            Ellipse(dc, box.left, box.top, box.right, box.bottom);
        } else {
            Arc(dc, box.left, box.top, box.right, box.bottom, x0, y0, x1, y1);
        }
    }
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(prog);

    std::wstring pct = percent >= 0 ? std::to_wstring(percent) + L"%" : L"...";
    Text(dc, g_fPct, t.text, pct, r, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}

void DrawGraph(HDC dc, const RECT& r, const Panel* p, const Theme& t) {
    if (r.bottom - r.top < Px(30)) return;
    RoundBox(dc, r, t.card, Px(10));
    if (p->history.size() < 2) return;

    float maxv = 1;
    for (float v : p->history) maxv = std::max(maxv, v);
    int w = r.right - r.left - Px(16), h = r.bottom - r.top - Px(16);
    int x0 = r.left + Px(8), yb = r.bottom - Px(8);

    std::vector<POINT> pts;
    pts.push_back({x0, yb});
    size_t n = p->history.size();
    for (size_t i = 0; i < n; i++) {
        int x = x0 + static_cast<int>(w * (static_cast<double>(i) / (n - 1)));
        int y = yb - static_cast<int>(h * (p->history[i] / maxv));
        pts.push_back({x, y});
    }
    pts.push_back({x0 + w, yb});

    HBRUSH fill = CreateSolidBrush(Blend(t.card, t.accent, 35));
    HPEN line = CreatePen(PS_SOLID, std::max(2, Px(2)), t.accent);
    HGDIOBJ ob = SelectObject(dc, fill), op = SelectObject(dc, line);
    Polygon(dc, pts.data(), static_cast<int>(pts.size()));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fill);
    DeleteObject(line);
}

void DrawButton(HDC dc, const RECT& r, const std::wstring& label, const Theme& t,
                bool primary) {
    RoundBox(dc, r, primary ? t.accent : t.card, Px(10));
    COLORREF c = primary ? RGB(255, 255, 255) : t.text;
    Text(dc, g_fBtn, c, label, r, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}

void Paint(HWND hwnd, Panel* p) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    Theme t = GetTheme();

    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    HBRUSH bg = CreateSolidBrush(t.bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    Layout L = MakeLayout(w, h);
    DrawRing(dc, L.ring, p->info.percent, t);

    int x = L.textLeft, right = L.textRight, y = L.top;
    std::wstring status = p->info.status.empty() ? L"Working..." : p->info.status;
    Text(dc, g_fTitle, t.text, status, RECT{x, y, right, y + Px(26)});
    Text(dc, g_fSmall, t.sub, p->info.file, RECT{x, y + Px(26), right, y + Px(48)});

    std::wstring speed = p->speed > 0 ? FormatSpeed(p->speed) : L"-";
    Text(dc, g_fBig, t.accent, speed, RECT{x, y + Px(54), right, y + Px(98)});

    std::wstring line1 = L"Time remaining: " + (p->info.eta.empty() ? L"-" : p->info.eta);
    std::wstring line2 = L"Items remaining: " + (p->info.items.empty() ? L"-" : p->info.items);
    Text(dc, g_fSmall, t.text, line1, RECT{x, y + Px(100), right, y + Px(122)});
    Text(dc, g_fSmall, t.text, line2, RECT{x, y + Px(122), right, y + Px(144)});

    if (g_settings.showGraph) DrawGraph(dc, L.graph, p, t);

    DrawButton(dc, L.btnPause, p->info.paused ? L"Resume" : L"Pause", t, false);
    DrawButton(dc, L.btnCancel, L"Cancel", t, true);

    BitBlt(hdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT:
            if (p) Paint(hwnd, p);
            else ValidateRect(hwnd, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONUP: {
            if (!p) return 0;
            RECT rc;
            GetClientRect(hwnd, &rc);
            Layout L = MakeLayout(rc.right, rc.bottom);
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            if (PtInRect(&L.btnPause, pt)) PressElement(p->pauseEl);
            else if (PtInRect(&L.btnCancel, pt)) PressElement(p->cancelEl);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------- dialog tracking

BOOL CALLBACK FindDialogsProc(HWND h, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<HWND>*>(lp);
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, kDialogClass) != 0 || !IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) out->push_back(h);
    return TRUE;
}

void DestroyPanel(Panel* p) {
    if (p->overlay) DestroyWindow(p->overlay);
    if (p->pauseEl) p->pauseEl->Release();
    if (p->cancelEl) p->cancelEl->Release();
    delete p;
}

void CreatePanel(HWND dialog) {
    auto* p = new Panel();
    p->dialog = dialog;
    p->overlay = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kOverlayClass, L"",
        WS_POPUP, 0, 0, 10, 10, dialog, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!p->overlay) {
        delete p;
        return;
    }
    SetWindowLongPtrW(p->overlay, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
    SetLayeredWindowAttributes(p->overlay, 0, static_cast<BYTE>(g_settings.opacity),
                               LWA_ALPHA);
    g_panels[dialog] = p;
}

void PlaceOverlay(Panel* p) {
    HWND content = FindWindowExW(p->dialog, nullptr, L"DirectUIHWND", nullptr);
    RECT r;
    bool ok = content && IsWindowVisible(p->dialog) && !IsIconic(p->dialog) &&
              GetWindowRect(content, &r);
    if (!ok) {
        ShowWindow(p->overlay, SW_HIDE);
        return;
    }
    SetWindowPos(p->overlay, HWND_TOP, r.left, r.top, r.right - r.left,
                 r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CALLBACK TickProc(HWND, UINT, UINT_PTR, DWORD) {
    std::vector<HWND> found;
    EnumWindows(FindDialogsProc, reinterpret_cast<LPARAM>(&found));

    for (HWND d : found) {
        if (!g_panels.count(d)) CreatePanel(d);
    }
    for (auto it = g_panels.begin(); it != g_panels.end();) {
        bool alive = std::find(found.begin(), found.end(), it->first) != found.end();
        if (!alive) {
            DestroyPanel(it->second);
            it = g_panels.erase(it);
        } else {
            ++it;
        }
    }

    bool scan = (++g_scanCounter % 3) == 0;
    Theme t = GetTheme();
    for (auto& kv : g_panels) {
        Panel* p = kv.second;
        if (!p->styled) StyleDialog(p, t);
        if (scan) {
            Scan(p);
            float mbps = static_cast<float>(p->speed / (1024.0 * 1024.0));
            p->history.push_back(mbps);
            if (p->history.size() > 90) p->history.erase(p->history.begin());
        }
        PlaceOverlay(p);
        InvalidateRect(p->overlay, nullptr, FALSE);
    }
}

void UiThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     IID_IUIAutomation, reinterpret_cast<void**>(&g_uia));

    g_scale = GetDpiForSystem() / 96.0;
    g_fTitle = MakeFont(Px(16), FW_SEMIBOLD);
    g_fBig = MakeFont(Px(30), FW_SEMIBOLD);
    g_fSmall = MakeFont(Px(13), FW_NORMAL);
    g_fPct = MakeFont(Px(30), FW_SEMIBOLD);
    g_fBtn = MakeFont(Px(14), FW_SEMIBOLD);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    RegisterClassExW(&wc);

    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_threadId = GetCurrentThreadId();

    UINT_PTR timer = SetTimer(nullptr, 0, kTickMs, TickProc);
    while (!g_stop && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (timer) KillTimer(nullptr, timer);

    for (auto& kv : g_panels) DestroyPanel(kv.second);
    g_panels.clear();
    UnregisterClassW(kOverlayClass, inst);
    DeleteObject(g_fTitle);
    DeleteObject(g_fBig);
    DeleteObject(g_fSmall);
    DeleteObject(g_fPct);
    DeleteObject(g_fBtn);
    if (g_uia) {
        g_uia->Release();
        g_uia = nullptr;
    }
    CoUninitialize();
}

}  // namespace

BOOL Wh_ModInit() {
    LoadSettings();
    g_stop = false;
    g_threadId = 0;
    g_thread = std::thread(UiThread);
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    g_stop = true;
    DWORD tid = g_threadId;
    if (tid) PostThreadMessageW(tid, WM_QUIT, 0, 0);
    if (g_thread.joinable()) g_thread.join();
}
