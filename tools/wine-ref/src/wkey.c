// wkey.exe click <title> <button>   |  wkey.exe key <vk-hex> [holdms]  (SendInput to TOMBRAIDER8 window)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static const char* want;
static BOOL CALLBACK child(HWND h, LPARAM out) {
    char t[256]; GetWindowTextA(h, t, sizeof t);
    if (!lstrcmpiA(t, want)) { *(HWND*)out = h; return FALSE; }
    return TRUE;
}
static HWND combos[16]; static int ncombos;
static BOOL CALLBACK combo(HWND h, LPARAM unused) {
    char c[64]; GetClassNameA(h, c, sizeof c);
    if (!lstrcmpiA(c, "ComboBox") && ncombos < 16) combos[ncombos++] = h;
    return TRUE;
}
int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "click")) {
        HWND w = FindWindowA(NULL, argv[2]);
        if (!w) { puts("no window"); return 1; }
        HWND b = 0; want = argv[3]; EnumChildWindows(w, child, (LPARAM)&b);
        if (!b) { puts("no button"); return 1; }
        SendMessageA(b, BM_CLICK, 0, 0); puts("clicked"); return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "combo")) { // combo <title> <index> <substring>
        HWND w = FindWindowA(NULL, argv[2]);
        if (!w) { puts("no window"); return 1; }
        EnumChildWindows(w, combo, 0);
        int n = atoi(argv[3]);
        if (n >= ncombos) { printf("only %d combos\n", ncombos); return 1; }
        HWND cb = combos[n]; int count = (int)SendMessageA(cb, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < count; ++i) {
            WCHAR wt[256]; char t[256]; SendMessageW(cb, CB_GETLBTEXT, i, (LPARAM)wt); WideCharToMultiByte(CP_UTF8, 0, wt, -1, t, sizeof t, 0, 0); printf("  [%d] %s\n", i, t);
            if (strstr(t, argv[4])) {
                SendMessageA(cb, CB_SETCURSEL, i, 0);
                SendMessageA(GetParent(cb), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(cb), CBN_SELCHANGE), (LPARAM)cb);
                printf("selected %d\n", i); return 0;
            }
        }
        return 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "post")) {
        HWND w = FindWindowA("TOMBRAIDER8", NULL);
        WORD vk = (WORD)strtol(argv[2], 0, 16);
        UINT sc = MapVirtualKeyA(vk, 0);
        LPARAM ext = (vk >= VK_LEFT && vk <= VK_DOWN) ? (1 << 24) : 0;
        PostMessageA(w, WM_KEYDOWN, vk, 1 | (sc << 16) | ext); Sleep(120);
        PostMessageA(w, WM_KEYUP, vk, 1 | (sc << 16) | ext | (3u << 30));
        printf("posted %x\n", vk); return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "km")) { // km <holdms> <totalms> vk...: hold keys, sample camera mode every 30 ms
        HWND w = FindWindowA("TOMBRAIDER8", NULL); if (w) SetForegroundWindow(w);
        DWORD pid; GetWindowThreadProcessId(w, &pid); HANDLE h = OpenProcess(PROCESS_VM_READ, FALSE, pid);
        int hold = atoi(argv[2]), total = atoi(argv[3]), n = argc - 4; INPUT in[8] = {0};
        for (int i = 0; i < n && i < 8; ++i) { WORD vk = (WORD)strtol(argv[4 + i], 0, 16);
            in[i].type = INPUT_KEYBOARD; in[i].ki.wVk = vk; in[i].ki.wScan = MapVirtualKeyA(vk, 0); in[i].ki.dwFlags = KEYEVENTF_SCANCODE; }
        SendInput(n, in, sizeof(INPUT)); int last = -999, lastr = -999, up = 0;
        for (int t = 0; t < total; t += 30) {
            if (!up && t >= hold) { for (int i = 0; i < n; ++i) in[i].ki.dwFlags |= KEYEVENTF_KEYUP; SendInput(n, in, sizeof(INPUT)); up = 1; }
            DWORD c = 0; int m = 0, r = 0; float L[3] = {0};
            ReadProcessMemory(h, (void*)0xE80534, &c, 4, 0); ReadProcessMemory(h, (void*)(c + 0x1e0), &m, 4, 0);
            ReadProcessMemory(h, (void*)(0xE804F0 + 0x58), &r, 4, 0);
            DWORD p = 0; ReadProcessMemory(h, (void*)0xE7F094, &p, 4, 0); ReadProcessMemory(h, (void*)(p + 0x20), L, 12, 0);
            if (m != last || r != lastr) { printf("t%d cam %d req %d L %.0f %.0f %.0f\n", t, m, r, L[0], L[1], L[2]); last = m; lastr = r; }
            Sleep(30);
        }
        if (!up) { for (int i = 0; i < n; ++i) in[i].ki.dwFlags |= KEYEVENTF_KEYUP; SendInput(n, in, sizeof(INPUT)); }
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "mouse")) { // mouse <dx> <dy> <steps>: relative moves 20 ms apart
        HWND w = FindWindowA("TOMBRAIDER8", NULL); if (w) SetForegroundWindow(w);
        int dx = atoi(argv[2]), dy = atoi(argv[3]), n = atoi(argv[4]);
        for (int i = 0; i < n; ++i) { INPUT in = {0}; in.type = INPUT_MOUSE; in.mi.dx = dx; in.mi.dy = dy; in.mi.dwFlags = MOUSEEVENTF_MOVE; SendInput(1, &in, sizeof in); Sleep(20); }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "multi")) { // multi <ms> vk...: hold several keys together
        HWND w = FindWindowA("TOMBRAIDER8", NULL); if (w) SetForegroundWindow(w);
        int hold = atoi(argv[2]), n = argc - 3; INPUT in[8] = {0};
        for (int i = 0; i < n && i < 8; ++i) { WORD vk = (WORD)strtol(argv[3 + i], 0, 16);
            in[i].type = INPUT_KEYBOARD; in[i].ki.wVk = vk; in[i].ki.wScan = MapVirtualKeyA(vk, 0); in[i].ki.dwFlags = KEYEVENTF_SCANCODE; }
        SendInput(n, in, sizeof(INPUT)); Sleep(hold);
        for (int i = 0; i < n; ++i) in[i].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(n, in, sizeof(INPUT)); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "key")) {
        HWND w = FindWindowA("TOMBRAIDER8", NULL);
        if (w) SetForegroundWindow(w);
        WORD vk = (WORD)strtol(argv[2], 0, 16);
        int hold = argc > 3 ? atoi(argv[3]) : 80;
        INPUT in[1] = {0};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk; in[0].ki.wScan = MapVirtualKeyA(vk, 0);
        in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) in[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, in, sizeof(INPUT)); Sleep(hold);
        in[0].ki.dwFlags |= KEYEVENTF_KEYUP; SendInput(1, in, sizeof(INPUT));
        printf("key %x to %p\n", vk, (void*)w); return 0;
    }
    return 2;
}
