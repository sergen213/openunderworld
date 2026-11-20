// wmem.exe unit <name>        one-shot: next level load (0x5DE2B0) goes to <name> instead
// wmem.exe mask               player instance (*0xE7F094) draw-group masks +0x23C/+0x240
// wmem.exe rd <hexaddr> <n>   hex dump      wmem.exe w32 <hexaddr> <hexval>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
// wbs <addr> <hexbytes>: wb with every thread of the game suspended and none inside [addr, addr + n) (patching a hot
// function's first instructions while it runs crashes it); retries up to 50 times.
static int patchSuspended(HANDLE h, DWORD a, const unsigned char* v, int n) {
    const DWORD pid = GetProcessId(h);
    for (int attempt = 0; attempt < 50; ++attempt) {
        HANDLE th[256]; int k = 0, clear = 1;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok && k < 256; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!t) continue;
            SuspendThread(t); th[k++] = t;
            CONTEXT c = {0}; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(t, &c) && c.Eip >= a && c.Eip < a + (DWORD)n) clear = 0;
        }
        CloseHandle(snap);
        int ok = 0;
        if (clear) {
            DWORD old; VirtualProtectEx(h, (void*)a, n, PAGE_EXECUTE_READWRITE, &old);
            ok = WriteProcessMemory(h, (void*)a, v, n, 0); FlushInstructionCache(h, (void*)a, n);
        }
        for (int i = 0; i < k; ++i) ResumeThread(th[i]), CloseHandle(th[i]);
        if (clear) return ok;
        Sleep(1);
    }
    return 0;
}
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
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    HANDLE h = proc();
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
    if (!strcmp(argv[1], "wbs") && argc > 3) { // wb with the game's threads suspended (patchSuspended)
        DWORD a = strtoul(argv[2], 0, 16); unsigned char v[64]; int n = 0; const char* q = argv[3];
        while (q[0] && q[1] && n < 64) { unsigned x; sscanf(q, "%2x", &x); v[n++] = (unsigned char)x; q += 2; }
        int ok = patchSuspended(h, a, v, n);
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
    if (!strcmp(argv[1], "inst")) { // instance list *0xD98D54 via +0x08: addr uid(+0x58) pos(+0x20) object name
        DWORD i = rd32(h, 0xD98D54); int guard = 0;
        while (i && guard++ < 4000) {
            float p[3]; ReadProcessMemory(h, (void*)(i + 0x20), p, 12, 0);
            DWORD obj = rd32(h, i + 0x10), np = obj ? rd32(h, obj + 0x60) : 0; char nm[48] = {0};
            if (np) ReadProcessMemory(h, (void*)np, nm, 47, 0);
            printf("%08lx %ld %.1f %.1f %.1f %s\n", i, (long)rd32(h, i + 0x58), p[0], p[1], p[2], nm);
            i = rd32(h, i + 8);
        }
        return 0;
    }
    if (!strcmp(argv[1], "state")) { // state: main state, front-end sub-state, the gameTracker's unit name, the player, Lara
        // mainTracker 0xE7ED60 +0 (MAIN_ProcessMainState 0x5E0090: 1 play, 2 level load, 9 paused frame, 10 front end);
        // front-end sub-state byte 0xFAE8DA (MAIN_FrontEnd 0x5DAE20: 0 legal, 1 intro movie, 2 press start, 3 main menu,
        // 4 starting a new game, 6 / 7 attract mode, 8 new-game movie); unit = gameTracker 0xE7F088 + 0x38; flags =
        // gameTracker + 0x2C (bit 0x10000000 while the pause menu is open [O Wine 2026-11-20: set on Escape in play,
        // cleared on the next])
        char nm[33] = {0}; ReadProcessMemory(h, (void*)0xE7F0C0, nm, 32, 0);
        unsigned char fe = 0; ReadProcessMemory(h, (void*)0xFAE8DA, &fe, 1, 0);
        DWORD p = rd32(h, 0xE7F094); float L[3] = {0};
        if (p) ReadProcessMemory(h, (void*)(p + 0x20), L, 12, 0);
        printf("main %lu fe %u flags %08lx unit %s player %08lx pos %.1f %.1f %.1f\n", rd32(h, 0xE7ED60), fe, rd32(h, 0xE7F0B4),
               nm[0] ? nm : "-", p, L[0], L[1], L[2]);
        return 0;
    }
    if (!strcmp(argv[1], "mask")) {
        DWORD p = rd32(h, 0xE7F094);
        printf("player %08lx mask23C %08lx mask240 %08lx\n", p, p ? rd32(h, p + 0x23C) : 0, p ? rd32(h, p + 0x240) : 0);
        return 0;
    }
    if (!strcmp(argv[1], "alloc")) { // alloc [bytes]: an RWX code cave (default one page)
        void* c = VirtualAllocEx(h, 0, argc > 2 ? strtoul(argv[2], 0, 0) : 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        printf("%08lx\n", (DWORD)(size_t)c); return 0;
    }
    if (!strcmp(argv[1], "cam")) { // cam [n] [ms]: Lara pos/rot, camera vtable, pos +0x90, pivot +0x100, desired +0x150, dist +0x1d4/+0x1320, mode +0x1e0
        int n = argc > 2 ? atoi(argv[2]) : 1, ms = argc > 3 ? atoi(argv[3]) : 100;
        for (int k = 0; k < n; ++k) {
            DWORD p = rd32(h, 0xE7F094), c = rd32(h, 0xE80534); float L[4], R[4], C[4], P[4], D[4], d[2];
            ReadProcessMemory(h, (void*)(p + 0x20), L, 16, 0); ReadProcessMemory(h, (void*)(p + 0x30), R, 16, 0);
            ReadProcessMemory(h, (void*)(c + 0x90), C, 16, 0); ReadProcessMemory(h, (void*)(c + 0x100), P, 16, 0);
            ReadProcessMemory(h, (void*)(c + 0x150), D, 16, 0); ReadProcessMemory(h, (void*)(c + 0x1d4), d, 4, 0);
            ReadProcessMemory(h, (void*)(c + 0x1320), d + 1, 4, 0);
            printf("L %.1f %.1f %.1f r %.3f %.3f %.3f vt %08lx C %.1f %.1f %.1f P %.1f %.1f %.1f D %.1f %.1f %.1f d %.1f %.1f m %ld ov %ld\n",
                   L[0], L[1], L[2], R[0], R[1], R[2], rd32(h, c), C[0], C[1], C[2], P[0], P[1], P[2], D[0], D[1], D[2], d[0], d[1],
                   (long)rd32(h, c + 0x1e0), (long)rd32(h, c + 0x1d0));
            fflush(stdout); if (k + 1 < n) Sleep(ms);
        }
        return 0;
    }
    if (!strcmp(argv[1], "rdp") && argc > 4) { // rdp <array> <count> <hex off>: per pointer in the array, the dword at +off
        DWORD a = strtoul(argv[2], 0, 16), off = strtoul(argv[4], 0, 16); int n = atoi(argv[3]);
        for (int i = 0; i < n; ++i) { DWORD p = rd32(h, a + 4 * i); printf("%d %08lx %08lx\n", i, p, p ? rd32(h, p + off) : 0); }
        return 0;
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
