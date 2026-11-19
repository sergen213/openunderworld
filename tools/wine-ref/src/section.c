// section.exe n ms : Lara's anim section 1 and her upper-body state chart, every ms (docs/GAMEPLAY.md "Script flinch
// overlay"). Per line: ms, position (+0x20), yaw (+0x38), the locomotion / upper-body charts' current state pointers
// (*(0xFADB58)+0xF10 / +0xF14, chart +0x510), the upper-body chart's pattern +0x488 ({u16 kind 1, u16 pattern}), the
// section count, section 0's weight and section 1's weight / step / target (+0xA4 / +0xA8 / +0xAC), then the dwords of
// section 1 (anim component *(inst+0x280), sections at +0x14, 0x320 bytes each) that changed since the last sample
// (all of them, and section 0's, on the first; +0x44 = the request playing, +0xD8 = its bone-mask runs).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static HANDLE h;
static DWORD rd32(DWORD a) { DWORD v = 0; ReadProcessMemory(h, (void*)a, &v, 4, 0); return v; }
static float rdf(DWORD a) { float v = 0; ReadProcessMemory(h, (void*)a, &v, 4, 0); return v; }
int main(int argc, char** argv) {
    HWND w = FindWindowA("TOMBRAIDER8", NULL);
    if (!w) { puts("no window"); return 1; }
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) { puts("no process"); return 1; }
    int n = argc > 1 ? atoi(argv[1]) : 1, ms = argc > 2 ? atoi(argv[2]) : 33;
    static DWORD prev[2][0x320 / 4];
    DWORD t0 = GetTickCount();
    for (int k = 0; k < n; ++k) {
        DWORD inst = rd32(0xE7F094), g = rd32(0xFADB58);
        DWORD c1 = rd32(g + 0xF10), c2 = rd32(g + 0xF14);
        DWORD comp = rd32(inst + 0x280), secs = rd32(comp + 0x14), nsec = rd32(comp + 0x18);
        printf("%lu pos %.1f %.1f %.1f yaw %.3f c1 %08lx c2 %08lx pat %08lx nsec %lu w0 %.3f w1 %.3f/%.3f/%.3f",
               GetTickCount() - t0, rdf(inst + 0x20), rdf(inst + 0x24), rdf(inst + 0x28), rdf(inst + 0x38),
               rd32(c1 + 0x510), rd32(c2 + 0x510), rd32(c2 + 0x488), nsec, rdf(secs + 0xA4),
               rdf(secs + 0x320 + 0xA4), rdf(secs + 0x320 + 0xA8), rdf(secs + 0x320 + 0xAC));
        for (int s = 0; s < 2 && s < (int)nsec; ++s) {
            DWORD cur[0x320 / 4];
            ReadProcessMemory(h, (void*)(secs + 0x320 * s), cur, sizeof cur, 0);
            if (s == 1 || k == 0) {
                printf(" | s%d", s);
                for (int i = 0; i < 0x320 / 4; ++i)
                    if (k == 0 || cur[i] != prev[s][i]) printf(" %x:%08lx", i * 4, cur[i]);
            }
            memcpy(prev[s], cur, sizeof cur);
        }
        puts("");
        fflush(stdout);
        if (k + 1 < n) Sleep(ms);
    }
    return 0;
}
