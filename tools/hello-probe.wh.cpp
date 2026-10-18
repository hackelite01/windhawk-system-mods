// ==WindhawkMod==
// @id              hello-probe
// @name            Hello Probe (diagnostic)
// @description     Writes a line to C:\Users\Public\hello-probe.txt when loaded into Explorer. Used to check that mods load at all.
// @version         0.1.0
// @author          hackelite01
// @github          https://github.com/hackelite01
// @include         explorer.exe
// @license         MIT
// ==/WindhawkMod==

#include <windows.h>

BOOL Wh_ModInit() {
    HANDLE h = CreateFileW(L"C:\\Users\\Public\\hello-probe.txt", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const char msg[] = "hello from Wh_ModInit\r\n";
        DWORD written = 0;
        WriteFile(h, msg, sizeof(msg) - 1, &written, nullptr);
        CloseHandle(h);
    }
    return TRUE;
}
