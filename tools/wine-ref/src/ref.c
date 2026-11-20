// ref.exe: measuring helper for tru.exe under Wine (work/waterentry-wine/weref.c, a superset of
// work/gameplaygoal/reference/ref2.c <- work/climbq/pt.c). Subcommands: drop, trace, rec, tp, tpv, god, mouse,
// unit, goto, wf, w8, wb, wp, call2, find, mask, rd, w32 (see the comment on each). tools/wine-ref/README.md.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define B(x) s[n++] = (unsigned char)(x)
#define D(x) do { DWORD v_ = (x); memcpy(s + n, &v_, 4); n += 4; } while (0)
static HANDLE proc(void) {
    HWND w = FindWindowA("TOMBRAIDER8", NULL);
    if (!w) { puts("no window"); exit(1); }
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) { puts("no process"); exit(1); }
    return h;
}
static DWORD rd32(HANDLE h, DWORD a) { DWORD v = 0; ReadProcessMemory(h, (void*)a, &v, 4, 0); return v; }
// One key of a schedule: vk 1 / 2 are the left / right mouse buttons (tru.exe: fire / aim), any other a keyboard key
// by scan code (arrows extended).
static void sendKey(int vk, int up) {
    INPUT in = {0};
    if (vk == 1 || vk == 2) {
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = vk == 1 ? (up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN) : (up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN);
    } else {
        in.type = INPUT_KEYBOARD; in.ki.wVk = (WORD)vk; in.ki.wScan = MapVirtualKeyA(vk, 0);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
        if (vk >= VK_LEFT && vk <= VK_DOWN) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(1, &in, sizeof in);
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    HANDLE h = proc();
    if (!strcmp(argv[1], "drop") && argc > 9) { // drop x y z pinms tracems vx vy vz [vk:down:up ...] (times from release)
        float v[3] = {(float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4])}; int pin = atoi(argv[5]), total = atoi(argv[6]);
        float v0[3] = {(float)atof(argv[7]), (float)atof(argv[8]), (float)atof(argv[9])}; float z0[3] = {0, 0, 0};
        int ne = argc - 10; WORD vk[32]; int dn[32], up[32], st[32] = {0};
        for (int i = 0; i < ne && i < 32; ++i) { unsigned k; sscanf(argv[10 + i], "%x:%d:%d", &k, &dn[i], &up[i]); vk[i] = (WORD)k; }
        HWND w = FindWindowA("TOMBRAIDER8", NULL); if (w) SetForegroundWindow(w);
        DWORD p = rd32(h, 0xE7F094), q = rd32(h, p + 0x288); timeBeginPeriod(1);
        float c[3] = {v[0], v[1], v[2] + 315.4f};
        LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        for (;;) { QueryPerformanceCounter(&t); if ((t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart > pin) break;
            WriteProcessMemory(h, (void*)(p + 0x20), v, 12, 0); WriteProcessMemory(h, (void*)(p + 0x2A0), z0, 12, 0);
            if (getenv("YAW")) { float y = (float)atof(getenv("YAW")); WriteProcessMemory(h, (void*)(p + 0x38), &y, 4, 0); }
            if (q) for (int o = 0; o < 3; ++o) WriteProcessMemory(h, (void*)(q + (o == 0 ? 0x640 : o == 1 ? 0x680 : 0x6D0)), c, 12, 0); Sleep(1); }
        WriteProcessMemory(h, (void*)(p + 0x20), v, 12, 0); WriteProcessMemory(h, (void*)(p + 0x2A0), v0, 12, 0);
        QueryPerformanceCounter(&t0);
        for (;;) {
            QueryPerformanceCounter(&t); double ms = (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            for (int i = 0; i < ne; ++i) if ((st[i] == 0 && ms >= dn[i]) || (st[i] == 1 && ms >= up[i])) {
                sendKey(vk[i], st[i]); printf("%.1f key %x %s\n", ms, vk[i], st[i] ? "up" : "down"); ++st[i]; }
            if (ms > total) break;
            { static int lifted; if (!lifted && getenv("LIFT")) { float lm, dz; sscanf(getenv("LIFT"), "%f:%f", &lm, &dz);
                if (ms >= lm) { float zz; ReadProcessMemory(h, (void*)(p + 0x28), &zz, 4, 0); zz += dz; WriteProcessMemory(h, (void*)(p + 0x28), &zz, 4, 0);
                    if (q) { float cz = zz + 315.4f; for (int o = 0; o < 3; ++o) WriteProcessMemory(h, (void*)(q + (o == 0 ? 0x648 : o == 1 ? 0x688 : 0x6D8)), &cz, 4, 0); }
                    printf("%.1f lift %g\n", ms, dz); lifted = 1; } } }
            DWORD cm = rd32(h, 0xE80534);
            if (cm && getenv("CAMFOLLOW")) { float dx, dy, dist, hz; sscanf(getenv("CAMFOLLOW"), "%f,%f,%f,%f", &dx, &dy, &dist, &hz); float lp[3];
                ReadProcessMemory(h, (void*)(p + 0x20), lp, 12, 0); float cp[3] = {lp[0] - dx * dist, lp[1] - dy * dist, lp[2] + hz};
                WriteProcessMemory(h, (void*)(cm + 0x90), cp, 12, 0); WriteProcessMemory(h, (void*)(cm + 0x150), cp, 12, 0); }
            float s[9] = {0}; ReadProcessMemory(h, (void*)(p + 0x20), s, 12, 0); ReadProcessMemory(h, (void*)(p + 0x30), s + 3, 12, 0);
            ReadProcessMemory(h, (void*)(p + 0x2A0), s + 6, 12, 0);
            DWORD pd = rd32(h, 0xFADB58), f10 = pd ? rd32(h, pd + 0xF10) : 0, stp = f10 ? rd32(h, f10 + 0x510) : 0;
            float wt = 0, sf = 0; if (pd) ReadProcessMemory(h, (void*)(pd + 0x8E0), &wt, 4, 0);
            sf = wt;
            { DWORD vi = pd ? rd32(h, pd + 0x8DC) : 0, a = vi ? rd32(h, vi + 0x2D0) : 0, b = a ? rd32(h, a) : 0, R = b ? rd32(h, b + 0x2EC) : 0;
              if (R) { float kx[4], ky[4], ph[4], hh[4]; unsigned char fl = 0; ReadProcessMemory(h, (void*)(R + 0x7C), &fl, 1, 0);
                ReadProcessMemory(h, (void*)(R + 0xD0), kx, 16, 0); ReadProcessMemory(h, (void*)(R + 0xE0), ky, 16, 0);
                ReadProcessMemory(h, (void*)(R + 0x120), ph, 16, 0); ReadProcessMemory(h, (void*)(R + 0x110), hh, 16, 0);
                if (fl & 2) for (int k = 0; k < 4; ++k) sf += hh[k] * ((float)sin(kx[k] * s[0] + ky[k] * s[1] + ph[k]) - 1.0f); } }
            if (pd) { float hp = 1000.0f; WriteProcessMemory(h, (void*)(pd + 0x628), &hp, 4, 0); }
            unsigned short an = 0; ReadProcessMemory(h, (void*)(p + 0xC18), &an, 2, 0);
            static float last[3]; static DWORD ls; static unsigned short la;
            if (memcmp(s, last, 12) || stp != ls || an != la) { memcpy(last, s, 12); ls = stp; la = an;
                printf("%.1f pos %.2f %.2f %.2f yaw %.4f vel %.2f %.2f %.2f st %08lx rq %d par %d an %u wt %.2f sf %.2f\n", ms, s[0], s[1], s[2], s[5], s[6], s[7], s[8], stp, (int)(rd32(h, stp + 4) & 0xffff), (int)(rd32(h, stp + 4) >> 16), an, wt, sf);
                fflush(stdout); }
            Sleep(1);
        }
        timeEndPeriod(1); return 0;
    }
    if (!strcmp(argv[1], "unit") && argc > 2) {
        unsigned char* cave = VirtualAllocEx(h, 0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        DWORD c = (DWORD)(size_t)cave, str = c + 64, flag = c + 60;
        unsigned char s[64]; int n = 0;
        B(0x80); B(0x3D); D(flag); B(0);            // cmp byte [flag],0
        B(0x75); B(15);                             // jne skip
        B(0xC6); B(0x05); D(flag); B(1);            // mov byte [flag],1
        B(0xC7); B(0x44); B(0x24); B(0x04); D(str); // mov [esp+4],str
        B(0x55); B(0x8B); B(0x6C); B(0x24); B(0x08); // skip: push ebp; mov ebp,[esp+8]
        B(0xE9); D(0x5DE2B5 - (c + n + 4));         // jmp back
        WriteProcessMemory(h, cave, s, n, 0);
        unsigned char z[64] = {0}; strncpy((char*)z, argv[2], 63);
        WriteProcessMemory(h, (void*)(size_t)str, z, 64, 0);
        unsigned char j[5] = {0xE9}; DWORD rel = c - (0x5DE2B0 + 5); memcpy(j + 1, &rel, 4);
        DWORD old; VirtualProtectEx(h, (void*)0x5DE2B0, 5, PAGE_EXECUTE_READWRITE, &old);
        if (!WriteProcessMemory(h, (void*)0x5DE2B0, j, 5, 0)) { puts("patch failed"); return 1; }
        FlushInstructionCache(h, (void*)0x5DE2B0, 5);
        printf("next load -> %s (cave %08lx)\n", argv[2], c); return 0;
    }
    if (!strcmp(argv[1], "goto") && argc > 2) { // GAMELOOP_RequestLevelChangeByName 0x5DF8C0(name, gameTracker, 4)
        unsigned char* cave = VirtualAllocEx(h, 0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        DWORD c = (DWORD)(size_t)cave, str = c + 64;
        unsigned char s[64]; int n = 0;
        B(0x6A); B(4); B(0x68); D(0xE7F088); B(0x68); D(str); B(0xB8); D(0x5DF8C0); B(0xFF); B(0xD0);
        B(0x83); B(0xC4); B(12); B(0x31); B(0xC0); B(0xC2); B(4); B(0);
        WriteProcessMemory(h, cave, s, n, 0);
        unsigned char z[64] = {0}; strncpy((char*)z, argv[2], 63);
        WriteProcessMemory(h, (void*)(size_t)str, z, 64, 0);
        HANDLE t = CreateRemoteThread(h, 0, 0, (LPTHREAD_START_ROUTINE)cave, 0, 0, 0);
        if (!t) { puts("thread failed"); return 1; }
        WaitForSingleObject(t, 5000); printf("requested %s\n", argv[2]); return 0;
    }
    if (!strcmp(argv[1], "wf") && argc > 3) { // write float
        DWORD a = strtoul(argv[2], 0, 16); float v = (float)atof(argv[3]);
        return !WriteProcessMemory(h, (void*)a, &v, 4, 0);
    }
    if (!strcmp(argv[1], "w8") && argc > 3) {
        DWORD a = strtoul(argv[2], 0, 16); unsigned char v = (unsigned char)strtoul(argv[3], 0, 16);
        return !WriteProcessMemory(h, (void*)a, &v, 1, 0);
    }
    if (!strcmp(argv[1], "wb") && argc > 3) { // write code bytes in one call: wb <addr> <hexbytes>
        DWORD a = strtoul(argv[2], 0, 16), old; unsigned char v[64]; int n = 0; const char* q = argv[3];
        while (q[0] && q[1] && n < 64) { unsigned x; sscanf(q, "%2x", &x); v[n++] = (unsigned char)x; q += 2; }
        VirtualProtectEx(h, (void*)a, n, PAGE_EXECUTE_READWRITE, &old);
        int ok = WriteProcessMemory(h, (void*)a, v, n, 0); FlushInstructionCache(h, (void*)a, n);
        printf("%d bytes %s\n", n, ok ? "ok" : "fail"); return !ok;
    }
    if (!strcmp(argv[1], "wp") && argc > 3) { // write code byte
        DWORD a = strtoul(argv[2], 0, 16); unsigned char v = (unsigned char)strtoul(argv[3], 0, 16), o = 0; DWORD old;
        ReadProcessMemory(h, (void*)a, &o, 1, 0);
        VirtualProtectEx(h, (void*)a, 1, PAGE_EXECUTE_READWRITE, &old);
        int ok = WriteProcessMemory(h, (void*)a, &v, 1, 0); FlushInstructionCache(h, (void*)a, 1);
        printf("%02x -> %02x %s\n", o, v, ok ? "ok" : "fail"); return !ok;
    }
    if (!strcmp(argv[1], "call2") && argc > 4) { // cdecl fn(a1, a2) on a remote thread
        unsigned char* cave = VirtualAllocEx(h, 0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        unsigned char s[64]; int n = 0;
        B(0x68); D(strtoul(argv[4], 0, 16)); B(0x68); D(strtoul(argv[3], 0, 16)); B(0xB8); D(strtoul(argv[2], 0, 16)); B(0xFF); B(0xD0);
        B(0x83); B(0xC4); B(8); B(0x31); B(0xC0); B(0xC2); B(4); B(0);
        WriteProcessMemory(h, cave, s, n, 0);
        HANDLE t = CreateRemoteThread(h, 0, 0, (LPTHREAD_START_ROUTINE)cave, 0, 0, 0);
        if (!t) { puts("thread failed"); return 1; }
        WaitForSingleObject(t, 5000); puts("called"); return 0;
    }
    if (!strcmp(argv[1], "find") && argc > 2) { // find <hex bytes>
        unsigned char pat[256]; int pn = 0; const char* q = argv[2];
        while (q[0] && q[1] && pn < 256) { unsigned v; sscanf(q, "%2x", &v); pat[pn++] = (unsigned char)v; q += 2; }
        MEMORY_BASIC_INFORMATION mi; unsigned char* a = 0; static unsigned char buf[1 << 20]; int hits = 0;
        while (VirtualQueryEx(h, a, &mi, sizeof mi) && (size_t)a < 0x7FFF0000u) {
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                for (size_t off = 0; off < mi.RegionSize; off += sizeof buf - 256) {
                    SIZE_T n = mi.RegionSize - off < sizeof buf ? mi.RegionSize - off : sizeof buf, got = 0;
                    ReadProcessMemory(h, (unsigned char*)mi.BaseAddress + off, buf, n, &got);
                    for (SIZE_T i = 0; i + pn <= got; ++i) if (buf[i] == pat[0] && !memcmp(buf + i, pat, pn)) {
                        printf("%08lx\n", (DWORD)(size_t)((unsigned char*)mi.BaseAddress + off + i)); if (++hits > 50) return 0; }
                    if (n < sizeof buf) break;
                }
            }
            a = (unsigned char*)mi.BaseAddress + mi.RegionSize;
        }
        return 0;
    }
    if (!strcmp(argv[1], "mask")) {
        DWORD p = rd32(h, 0xE7F094);
        printf("player %08lx mask23C %08lx mask240 %08lx\n", p, p ? rd32(h, p + 0x23C) : 0, p ? rd32(h, p + 0x240) : 0);
        return 0;
    }
    if (!strcmp(argv[1], "mouse") && argc > 3) { // mouse dx dy [steps]: relative mouse motion (camera orbit)
        HWND w = FindWindowA("TOMBRAIDER8", NULL); if (w) SetForegroundWindow(w);
        int n = argc > 4 ? atoi(argv[4]) : 10;
        for (int i = 0; i < n; ++i) { INPUT in = {0}; in.type = INPUT_MOUSE; in.mi.dx = atoi(argv[2]) / n; in.mi.dy = atoi(argv[3]) / n;
            in.mi.dwFlags = MOUSEEVENTF_MOVE; SendInput(1, &in, sizeof in); Sleep(20); }
        return 0;
    }
    if (!strcmp(argv[1], "tp") && argc > 5) { // tp x y z ms : rewrite player pos (+0x20) every ms for ms
        float v[3] = {(float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4])}; int ms = atoi(argv[5]);
        DWORD p = rd32(h, 0xE7F094), q = rd32(h, p + 0x288); timeBeginPeriod(1);
        float c[3] = {v[0], v[1], v[2] + 315.4f}; // proxy capsule centre (+0x640/+0x680/+0x6D0)
        LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); // ms of wall time, not loops
        for (;;) { QueryPerformanceCounter(&t); if ((t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart > ms) break;
            WriteProcessMemory(h, (void*)(p + 0x20), v, 12, 0);
            if (q) for (int o = 0; o < 3; ++o) WriteProcessMemory(h, (void*)(q + (o == 0 ? 0x640 : o == 1 ? 0x680 : 0x6D0)), c, 12, 0); Sleep(1); }
        timeEndPeriod(1); return 0;
    }
    if (!strcmp(argv[1], "tpv") && argc > 5) { // tpv x y z ms : as tp, and zero the velocity (+0x2A0) each ms
        float v[3] = {(float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4])}; int ms = atoi(argv[5]); float z[3] = {0, 0, 0};
        DWORD p = rd32(h, 0xE7F094), q = rd32(h, p + 0x288); timeBeginPeriod(1);
        float c[3] = {v[0], v[1], v[2] + 315.4f};
        LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); // ms of wall time, not loops
        for (;;) { QueryPerformanceCounter(&t); if ((t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart > ms) break;
            WriteProcessMemory(h, (void*)(p + 0x20), v, 12, 0); WriteProcessMemory(h, (void*)(p + 0x2A0), z, 12, 0);
            if (q) for (int o = 0; o < 3; ++o) WriteProcessMemory(h, (void*)(q + (o == 0 ? 0x640 : o == 1 ? 0x680 : 0x6D0)), c, 12, 0); Sleep(1); }
        timeEndPeriod(1); return 0;
    }
    if (!strcmp(argv[1], "god")) { // god: keep health full until killed
        for (;;) { DWORD pd = rd32(h, 0xFADB58); if (pd) { float hp = 1000.0f; WriteProcessMemory(h, (void*)(pd + 0x628), &hp, 4, 0); } Sleep(2); }
    }
    if (!strcmp(argv[1], "trace") && argc > 2) { // trace <ms> [vk:downms:upms ...] : keys + per-change player samples
        HWND w = FindWindowA("TOMBRAIDER8", NULL); if (w) SetForegroundWindow(w);
        int total = atoi(argv[2]), ne = argc - 3; WORD vk[32]; int dn[32], up[32], st[32] = {0}; double at[32] = {0};
        for (int i = 0; i < ne && i < 32; ++i) { unsigned v; sscanf(argv[3 + i], "%x:%d:%d", &v, &dn[i], &up[i]); vk[i] = (WORD)v; }
        LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        float last[6] = {0}; timeBeginPeriod(1);
        for (;;) {
            QueryPerformanceCounter(&t); double ms = (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            for (int i = 0; i < ne; ++i) if ((st[i] == 0 && ms >= dn[i]) || (st[i] == 1 && ms >= up[i] && ms >= at[i] + (up[i] - dn[i]))) { if (st[i] == 0) at[i] = ms;
                sendKey(vk[i], st[i]); printf("%.1f key %x %s\n", ms, vk[i], st[i] ? "up" : "down"); ++st[i];
            }
            if (ms > total) break;
            DWORD p = rd32(h, 0xE7F094), c = rd32(h, 0xE80534); float s[14] = {0};
            if (p && getenv("LOCKYAW") && ms < atof(getenv("LOCKUNTIL") ? getenv("LOCKUNTIL") : "1e9")) { float y = (float)atof(getenv("LOCKYAW")); WriteProcessMemory(h, (void*)(p + 0x38), &y, 4, 0);
                if (getenv("LOCKCAM")) { float cy[2] = {(float)atof(getenv("CAMYAW") ? getenv("CAMYAW") : getenv("LOCKYAW")), 0}; cy[1] = cy[0]; WriteProcessMemory(h, (void*)strtoul(getenv("LOCKCAM"), 0, 16), cy, 8, 0); } }
            if (p && c && getenv("CAMFOLLOW") && ms < atof(getenv("CAMUNTIL") ? getenv("CAMUNTIL") : "1e9")) { // camera pinned behind her: input forward = (dx,dy)
                float dx, dy, dist, hz; sscanf(getenv("CAMFOLLOW"), "%f,%f,%f,%f", &dx, &dy, &dist, &hz); float lp[3];
                ReadProcessMemory(h, (void*)(p + 0x20), lp, 12, 0);
                float cp[3] = {lp[0] - dx * dist, lp[1] - dy * dist, lp[2] + hz};
                WriteProcessMemory(h, (void*)(c + 0x90), cp, 12, 0); WriteProcessMemory(h, (void*)(c + 0x150), cp, 12, 0); }
            if (p) { ReadProcessMemory(h, (void*)(p + 0x20), s, 12, 0); ReadProcessMemory(h, (void*)(p + 0x30), s + 3, 12, 0);
                     ReadProcessMemory(h, (void*)(p + 0x2A0), s + 6, 12, 0); }
            if (c) ReadProcessMemory(h, (void*)(c + 0x90), s + 9, 12, 0);
            DWORD pd = rd32(h, 0xFADB58), f10 = pd ? rd32(h, pd + 0xF10) : 0, stp = f10 ? rd32(h, f10 + 0x510) : 0;
            unsigned short an = 0; float ex[4] = {0}; if (p) { ReadProcessMemory(h, (void*)(p + 0xC18), &an, 2, 0); ReadProcessMemory(h, (void*)(p + 0xC1C), ex, 16, 0); }
            static DWORD lastSt; static unsigned short lastAn;
            if (pd && getenv("GOD")) { float hp[2] = {1000.0f, 0.0f}; WriteProcessMemory(h, (void*)(pd + 0x628), hp, 8, 0); }
            if (memcmp(s, last, 24) || stp != lastSt || an != lastAn || getenv("ALL")) { memcpy(last, s, 24); lastSt = stp; lastAn = an;
                printf("%.1f pos %.2f %.2f %.2f rot %.4f %.4f %.4f vel %.2f %.2f %.2f st %08lx an %u ex %g %g %g %g\n", ms,
                       s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], stp, an, ex[0], ex[1], ex[2], ex[3]); fflush(stdout); }
            Sleep(1);
        }
        timeEndPeriod(1); return 0;
    }
    if (!strcmp(argv[1], "rec") && argc > 2) { // rec <ms> [vk:downms:upms ...] : keys + per-change samples of player, water, breath, health, camera
        // env: GOD (health kept full), CAMFOLLOW=dx,dy,dist,h + CAMUNTIL=ms (camera pinned behind her), LOCKYAW=rad + LOCKUNTIL=ms
        // line: ms pos x y z rot rx ry rz vel vx vy vz (units per 30 Hz frame) st <state> rq <request> par <parent> an <anim>
        //       wt <volume top, player +0x8E0> sf <wt + tru.exe's sine waves at her x, y> air <player +0x920, frames>
        //       hp <player +0x628> cam <camera +0x90> piv <pivot +0x100> d <distance +0x1D4> m <camera mode +0x1E0>
        //       lk <camera +0x160: look point - camera, 0x6009C0> mx <camera matrix +0x10: rows 0..2, xyz each>
        //       PDX=hexoff: a line "ms px <value>" before each sample, the dword at player data (DAT_00fadb58) + off
        HWND w = FindWindowA("TOMBRAIDER8", NULL); if (w) SetForegroundWindow(w);
        int total = atoi(argv[2]), ne = argc - 3; WORD vk[32]; int dn[32], up[32], st[32] = {0}; double at[32] = {0};
        for (int i = 0; i < ne && i < 32; ++i) { unsigned v; sscanf(argv[3 + i], "%x:%d:%d", &v, &dn[i], &up[i]); vk[i] = (WORD)v; }
        LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        float last[14] = {0}; DWORD lastSt = 0; unsigned short lastAn = 0; long lastM = -2; DWORD lastPx = 0; timeBeginPeriod(1);
        for (;;) {
            QueryPerformanceCounter(&t); double ms = (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            for (int i = 0; i < ne; ++i) if ((st[i] == 0 && ms >= dn[i]) || (st[i] == 1 && ms >= up[i] && ms >= at[i] + (up[i] - dn[i]))) { if (st[i] == 0) at[i] = ms;
                sendKey(vk[i], st[i]); printf("%.1f key %x %s\n", ms, vk[i], st[i] ? "up" : "down"); ++st[i];
            }
            if (ms > total) break;
            DWORD p = rd32(h, 0xE7F094), c = rd32(h, 0xE80534), pd = rd32(h, 0xFADB58);
            if (p && getenv("LOCKYAW") && ms < atof(getenv("LOCKUNTIL") ? getenv("LOCKUNTIL") : "1e9")) { float y = (float)atof(getenv("LOCKYAW")); WriteProcessMemory(h, (void*)(p + 0x38), &y, 4, 0); }
            if (p && c && getenv("CAMFOLLOW") && ms < atof(getenv("CAMUNTIL") ? getenv("CAMUNTIL") : "1e9")) {
                float dx, dy, dist, hz; sscanf(getenv("CAMFOLLOW"), "%f,%f,%f,%f", &dx, &dy, &dist, &hz); float lp[3];
                ReadProcessMemory(h, (void*)(p + 0x20), lp, 12, 0);
                float cp[3] = {lp[0] - dx * dist, lp[1] - dy * dist, lp[2] + hz};
                WriteProcessMemory(h, (void*)(c + 0x90), cp, 12, 0); WriteProcessMemory(h, (void*)(c + 0x150), cp, 12, 0); }
            if (pd && getenv("GOD")) { float hp = 1000.0f; WriteProcessMemory(h, (void*)(pd + 0x628), &hp, 4, 0); }
            float s[9] = {0}, C[3] = {0}, P[3] = {0}, L[3] = {0}, M[12] = {0}, dist = 0, wt = 0, sf, air = 0, hp = 0; long m = -1;
            if (p) { ReadProcessMemory(h, (void*)(p + 0x20), s, 12, 0); ReadProcessMemory(h, (void*)(p + 0x30), s + 3, 12, 0);
                     ReadProcessMemory(h, (void*)(p + 0x2A0), s + 6, 12, 0); }
            if (c) { ReadProcessMemory(h, (void*)(c + 0x90), C, 12, 0); ReadProcessMemory(h, (void*)(c + 0x100), P, 12, 0);
                     ReadProcessMemory(h, (void*)(c + 0x1D4), &dist, 4, 0); m = (long)rd32(h, c + 0x1E0);
                     ReadProcessMemory(h, (void*)(c + 0x160), L, 12, 0); ReadProcessMemory(h, (void*)(c + 0x10), M, 48, 0); }
            DWORD f10 = pd ? rd32(h, pd + 0xF10) : 0, stp = f10 ? rd32(h, f10 + 0x510) : 0;
            if (pd) { ReadProcessMemory(h, (void*)(pd + 0x8E0), &wt, 4, 0); ReadProcessMemory(h, (void*)(pd + 0x920), &air, 4, 0);
                      ReadProcessMemory(h, (void*)(pd + 0x628), &hp, 4, 0); }
            sf = wt;
            { DWORD vi = pd ? rd32(h, pd + 0x8DC) : 0, a = vi ? rd32(h, vi + 0x2D0) : 0, b = a ? rd32(h, a) : 0, R = b ? rd32(h, b + 0x2EC) : 0;
              if (R) { float kx[4], ky[4], ph[4], hh[4]; unsigned char fl = 0; ReadProcessMemory(h, (void*)(R + 0x7C), &fl, 1, 0);
                ReadProcessMemory(h, (void*)(R + 0xD0), kx, 16, 0); ReadProcessMemory(h, (void*)(R + 0xE0), ky, 16, 0);
                ReadProcessMemory(h, (void*)(R + 0x120), ph, 16, 0); ReadProcessMemory(h, (void*)(R + 0x110), hh, 16, 0);
                if (fl & 2) for (int k = 0; k < 4; ++k) sf += hh[k] * ((float)sin(kx[k] * s[0] + ky[k] * s[1] + ph[k]) - 1.0f); } }
            unsigned short an = 0; if (p) ReadProcessMemory(h, (void*)(p + 0xC18), &an, 2, 0);
            // health and whole breath frames count as changes too: a still body under water still logs its drain and drowning
            float cur[14] = {s[0], s[1], s[2], s[3], s[4], s[5], C[0], C[1], C[2], P[0], P[1], P[2], hp, (float)(int)air};
            // PDX=hex: also log the player-data dword at that offset (DAT_00fadb58 + off; e.g. EDC = clearance result)
            DWORD px = (pd && getenv("PDX")) ? rd32(h, pd + strtoul(getenv("PDX"), 0, 16)) : 0;
            if (memcmp(cur, last, sizeof cur) || stp != lastSt || an != lastAn || m != lastM || px != lastPx) {
                memcpy(last, cur, sizeof cur); lastSt = stp; lastAn = an; lastM = m; lastPx = px;
                if (getenv("PDX")) printf("%.1f px %lu\n", ms, px);
                printf("%.1f pos %.2f %.2f %.2f rot %.4f %.4f %.4f vel %.2f %.2f %.2f st %08lx rq %d par %d an %u wt %.2f sf %.2f air %.1f hp %.1f"
                       " cam %.1f %.1f %.1f piv %.1f %.1f %.1f d %.1f m %ld lk %.2f %.2f %.2f mx %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
                       ms, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], stp,
                       stp ? (int)(rd32(h, stp + 4) & 0xffff) : -1, stp ? (int)(rd32(h, stp + 4) >> 16) : -1, an, wt, sf, air, hp,
                       C[0], C[1], C[2], P[0], P[1], P[2], dist, m, L[0], L[1], L[2], M[0], M[1], M[2], M[4], M[5], M[6], M[8], M[9], M[10]);
                fflush(stdout); }
            Sleep(1);
        }
        timeEndPeriod(1); return 0;
    }
    if (!strcmp(argv[1], "rd") && argc > 3) {
        DWORD a = strtoul(argv[2], 0, 16); int len = atoi(argv[3]);
        unsigned char b[4096]; if (len > 4096) len = 4096;
        SIZE_T got = 0; ReadProcessMemory(h, (void*)a, b, len, &got);
        for (int i = 0; i < (int)got; ++i) printf("%02x%s", b[i], (i & 15) == 15 ? "\n" : " ");
        puts(""); return 0;
    }
    if (!strcmp(argv[1], "w32") && argc > 3) {
        DWORD a = strtoul(argv[2], 0, 16), v = strtoul(argv[3], 0, 16);
        return !WriteProcessMemory(h, (void*)a, &v, 4, 0);
    }
    return 2;
}
