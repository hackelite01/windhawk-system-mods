// ==WindhawkMod==
// @id              file-operation-probe
// @name            File Operation Dialog Probe (diagnostic)
// @description     Logs the window and UI Automation structure of the Explorer copy/move/delete dialog. Diagnostic only, changes nothing.
// @version         0.1.0
// @author          hackelite01
// @github          https://github.com/hackelite01
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -luuid -luser32 -ldwmapi
// @license         MIT
// ==/WindhawkMod==

// Read-only probe. Open the Windhawk editor for this mod and watch the log
// output while you copy a large file in Explorer.

#include <windows.h>
#include <dwmapi.h>
#include <uiautomation.h>

#include <atomic>
#include <cstdarg>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <thread>

namespace {

// Appends to C:\Users\Public\file-operation-probe.log.
void ProbeLog(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;

    HANDLE h = CreateFileW(L"C:\\Users\\Public\\file-operation-probe.log",
                           FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::wstring line = std::wstring(buf) + L"\r\n";
    char utf8[8192];
    int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                  static_cast<int>(line.size()), utf8,
                                  sizeof(utf8), nullptr, nullptr);
    DWORD written = 0;
    if (len > 0) WriteFile(h, utf8, len, &written, nullptr);
    CloseHandle(h);
}

std::thread g_thread;
std::atomic<bool> g_stop{false};

struct Seen {
    DWORD firstTick = 0;
    DWORD lastDump = 0;
    std::wstring lastTitle;
    int dumps = 0;
};

std::wstring Indent(int depth) { return std::wstring(depth * 2, L' '); }

std::wstring WinClass(HWND h) {
    wchar_t buf[256] = L"";
    GetClassNameW(h, buf, 256);
    return buf;
}

std::wstring WinText(HWND h) {
    wchar_t buf[256] = L"";
    GetWindowTextW(h, buf, 256);
    return buf;
}

BOOL CALLBACK ChildProc(HWND child, LPARAM) {
    RECT cr{};
    GetWindowRect(child, &cr);
    ProbeLog(L"[probe]   CHILD hwnd=%p class='%s' text='%s' visible=%d "
             L"rect=%ld,%ld,%ld,%ld",
             child, WinClass(child).c_str(), WinText(child).c_str(),
             IsWindowVisible(child) ? 1 : 0, cr.left, cr.top, cr.right,
             cr.bottom);
    return TRUE;
}

void DumpWindow(HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    ProbeLog(L"[probe] TOP hwnd=%p class='%s' title='%s' pid=%u tid=%u "
           L"rect=%ld,%ld,%ld,%ld style=%08lx ex=%08lx",
           hwnd, WinClass(hwnd).c_str(), WinText(hwnd).c_str(), pid, tid,
           r.left, r.top, r.right, r.bottom,
           static_cast<unsigned long>(GetWindowLongW(hwnd, GWL_STYLE)),
           static_cast<unsigned long>(GetWindowLongW(hwnd, GWL_EXSTYLE)));

    EnumChildWindows(hwnd, ChildProc, 0);
}

void DumpUia(IUIAutomationTreeWalker* walker, IUIAutomationElement* el,
             int depth) {
    if (!el || depth > 12) return;

    BSTR name = nullptr, cls = nullptr, aid = nullptr;
    CONTROLTYPEID ct = 0;
    el->get_CurrentName(&name);
    el->get_CurrentClassName(&cls);
    el->get_CurrentAutomationId(&aid);
    el->get_CurrentControlType(&ct);

    double value = -1;
    IUIAutomationRangeValuePattern* range = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(
            UIA_RangeValuePatternId, IID_IUIAutomationRangeValuePattern,
            reinterpret_cast<void**>(&range))) &&
        range) {
        range->get_CurrentValue(&value);
        range->Release();
    }

    ProbeLog(L"[probe] %sUIA type=%d class='%s' id='%s' name='%s' range=%.2f",
           Indent(depth).c_str(), static_cast<int>(ct), cls ? cls : L"",
           aid ? aid : L"", name ? name : L"", value);

    SysFreeString(name);
    SysFreeString(cls);
    SysFreeString(aid);

    IUIAutomationElement* child = nullptr;
    walker->GetFirstChildElement(el, &child);
    while (child) {
        DumpUia(walker, child, depth + 1);
        IUIAutomationElement* next = nullptr;
        walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = next;
    }
}

void DumpUiaForWindow(IUIAutomation* uia, HWND hwnd) {
    IUIAutomationElement* root = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    if (SUCCEEDED(uia->ElementFromHandle(hwnd, &root)) && root &&
        SUCCEEDED(uia->get_ControlViewWalker(&walker)) && walker) {
        DumpUia(walker, root, 0);
    } else {
        ProbeLog(L"[probe] UIA could not open hwnd=%p", hwnd);
    }
    if (walker) walker->Release();
    if (root) root->Release();
}

// Collect top-level windows that look like the file operation dialog.
// The class name is not certain, so match a few likely candidates and also
// anything whose title looks like "% complete".
BOOL CALLBACK CollectProc(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::map<HWND, int>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    std::wstring cls = WinClass(hwnd);
    std::wstring title = WinText(hwnd);
    bool match = cls == L"OperationStatusWindow" ||
                 cls.find(L"Operation") != std::wstring::npos ||
                 title.find(L"complete") != std::wstring::npos ||
                 title.find(L"Calculating") != std::wstring::npos;
    if (match) (*out)[hwnd] = 1;
    return TRUE;
}

BOOL CALLBACK LogNewTopProc(HWND h, LPARAM lp) {
    auto* logged = reinterpret_cast<std::set<HWND>*>(lp);
    if (!IsWindowVisible(h) || logged->count(h)) return TRUE;
    logged->insert(h);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    ProbeLog(L"[probe] new window hwnd=%p class='%s' title='%s' pid=%u", h,
             WinClass(h).c_str(), WinText(h).c_str(), pid);
    return TRUE;
}

void ProbeThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     IID_IUIAutomation, reinterpret_cast<void**>(&uia));
    ProbeLog(L"[probe] started, UIA=%s", uia ? L"ok" : L"FAILED");

    std::map<HWND, Seen> seen;
    std::set<HWND> loggedTop;
    int heartbeat = 0;
    while (!g_stop) {
        std::map<HWND, int> now;
        EnumWindows(CollectProc, reinterpret_cast<LPARAM>(&now));

        // Log every visible top-level window once, so the real dialog class
        // shows up even if CollectProc's guesses are wrong.
        EnumWindows(LogNewTopProc, reinterpret_cast<LPARAM>(&loggedTop));

        if (++heartbeat % 300 == 0) ProbeLog(L"[probe] alive");

        DWORD tick = GetTickCount();
        for (auto& [hwnd, unused] : now) {
            Seen& s = seen[hwnd];
            if (s.firstTick == 0) s.firstTick = tick;
            // Dump on first sight, then whenever the title (which carries
            // the percentage) changes, at most every 400 ms and 12 times.
            std::wstring title = WinText(hwnd);
            if (s.dumps < 12 && (s.dumps == 0 || (title != s.lastTitle &&
                                                  tick - s.lastDump >= 400))) {
                s.dumps++;
                s.lastTitle = title;
                s.lastDump = tick;
                ProbeLog(L"[probe] ===== dump %d of hwnd=%p =====", s.dumps, hwnd);
                DumpWindow(hwnd);
                if (uia) DumpUiaForWindow(uia, hwnd);
                ProbeLog(L"[probe] ===== end dump =====");
            }
        }
        // Forget windows that are gone.
        for (auto it = seen.begin(); it != seen.end();) {
            it = now.count(it->first) ? std::next(it) : seen.erase(it);
        }
        Sleep(100);
    }

    if (uia) uia->Release();
    CoUninitialize();
}

}  // namespace

BOOL Wh_ModInit() {
    ProbeLog(L"[probe] Wh_ModInit pid=%u", GetCurrentProcessId());
    g_stop = false;
    g_thread = std::thread(ProbeThread);
    return TRUE;
}

void Wh_ModUninit() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}
