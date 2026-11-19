// enemy.exe: watch enemies around Lara in tru.exe (OU-CMB-003, shark aggression).
//  enemy list                       enemy-family instances (oflags2 0x400000): uid, position, object name
//  enemy watch <ms> <period> uid... every period ms: Lara position, health and per enemy uid its position, heading,
//                                   distance to Lara and the EnemyBehavior / EnemyState objects its AI holds
//                                   (AI = **(inst+0x2D0)+0x2EC; words of the AI block pointing at an object whose
//                                   vftable lies in the behaviour / state RTTI range 0x856000..0x85B000), printed when
//                                   they change. A health drop is logged as "hurt <amount>"; health under 600 is
//                                   put back to 1000 (logged "heal") so that she survives a long watch.
//  enemy zones uid...               per enemy its guard zones as 0x622C80 tests them: AI+0x2C90 block (0x61F810:
//                                   count +0xC0 of {point +0x30 + 16 k, inner radius s32 +0x90 + 4 k, outer +0xA8 + 4 k},
//                                   half-planes count +0xF0 of {point +0xD0 + 16 k, normal +0xE0 + 16 k}) and the region
//                                   index AI+0x3EB8 into the table *0xE92008 (0x58-byte entries: s32 count, instance,
//                                   radius +0x14, half height +0x18 printed as raw words; 0x620BE0), with Lara's position.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static HANDLE h;
static DWORD rd32(DWORD a) { DWORD v = 0; ReadProcessMemory(h, (void*)(size_t)a, &v, 4, 0); return v; }
static float rdf(DWORD a) { float v = 0; ReadProcessMemory(h, (void*)(size_t)a, &v, 4, 0); return v; }
static DWORD find(long uid) {
    DWORD i = rd32(0xD98D54); int g = 0;
    while (i && g++ < 6000) { if ((long)rd32(i + 0x58) == uid) return i; i = rd32(i + 8); }
    return 0;
}
static void name(DWORD i, char* nm) {
    DWORD obj = rd32(i + 0x10), np = obj ? rd32(obj + 0x60) : 0;
    memset(nm, 0, 48);
    if (np) ReadProcessMemory(h, (void*)(size_t)np, nm, 47, 0);
}
#define AI_SIZE 0x5100
#define MAXE 16
static DWORD aiBuf[AI_SIZE / 4];
static char last[MAXE][4096];
static void states(DWORD inst, char* out, size_t cap) {
    out[0] = 0;
    DWORD c = rd32(inst + 0x2D0), ai = c ? rd32(rd32(c) + 0x2EC) : 0;
    if (!ai || !ReadProcessMemory(h, (void*)(size_t)ai, aiBuf, sizeof aiBuf, 0)) return;
    size_t n = 0;
    for (int k = 0; k < AI_SIZE / 4 && n + 24 < cap; ++k) {
        DWORD p = aiBuf[k];
        if (p < 0x10000 || (p >= 0x400000 && p < 0x1000000)) continue;
        DWORD vt = rd32(p);
        if (vt >= 0x856000 && vt < 0x85B000) n += (size_t)snprintf(out + n, cap - n, " %x:%lx", k * 4, vt);
    }
}
int main(int argc, char** argv) {
    HWND w = FindWindowA("TOMBRAIDER8", NULL);
    if (!w) { puts("no window"); return 1; }
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (argc > 1 && !strcmp(argv[1], "list")) {
        DWORD i = rd32(0xD98D54); int g = 0; char nm[48];
        while (i && g++ < 6000) {
            DWORD obj = rd32(i + 0x10), base = obj ? rd32(obj + 0x0) : 0;
            (void)base;
            name(i, nm);
            DWORD c = rd32(i + 0x2D0);
            if (c) printf("%ld %08lx %.1f %.1f %.1f %s\n", (long)rd32(i + 0x58), i, rdf(i + 0x20), rdf(i + 0x24), rdf(i + 0x28), nm);
            i = rd32(i + 8);
        }
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "zones")) {
        DWORD L = rd32(0xE7F094), table = rd32(0xE92008);
        printf("lara %.1f %.1f %.1f up %.3f %.3f %.3f\n", rdf(L + 0x20), rdf(L + 0x24), rdf(L + 0x28), rdf(0xECFB00), rdf(0xECFB04), rdf(0xECFB08));
        for (int k = 2; k < argc; ++k) {
            DWORD i = find(atol(argv[k])), c = i ? rd32(i + 0x2D0) : 0, ai = c ? rd32(rd32(c) + 0x2EC) : 0;
            if (!ai) { printf("%s no AI\n", argv[k]); continue; }
            DWORD z = ai + 0x2C90; int n = (int)rd32(z + 0xC0), np = (int)rd32(z + 0xF0), reg = (int)rd32(ai + 0x3EB8);
            printf("%s ai %08lx zones %d planes %d region %d\n", argv[k], ai, n, np, reg);
            for (int j = 0; j < n && j < 6; ++j)
                printf("  zone %d point %.1f %.1f %.1f inner %ld outer %ld\n", j, rdf(z + 0x30 + 16 * j), rdf(z + 0x34 + 16 * j), rdf(z + 0x38 + 16 * j),
                       (long)rd32(z + 0x90 + 4 * j), (long)rd32(z + 0xA8 + 4 * j));
            for (int j = 0; j < np && j < 4; ++j)
                printf("  plane %d point %.1f %.1f %.1f normal %.3f %.3f %.3f\n", j, rdf(z + 0xD0 + 16 * j), rdf(z + 0xD4 + 16 * j), rdf(z + 0xD8 + 16 * j),
                       rdf(z + 0xE0 + 16 * j), rdf(z + 0xE4 + 16 * j), rdf(z + 0xE8 + 16 * j));
            if (reg != -1 && table) {
                DWORD e = table + 0x58 * (DWORD)reg, inst = rd32(e + 4);
                printf("  region count %ld inst %08lx uid %ld at %.1f %.1f %.1f radius %08lx half %08lx\n", (long)rd32(e), inst, inst ? (long)rd32(inst + 0x58) : 0,
                       inst ? rdf(inst + 0x20) : 0, inst ? rdf(inst + 0x24) : 0, inst ? rdf(inst + 0x28) : 0, rd32(e + 0x14), rd32(e + 0x18));
            }
        }
        return 0;
    }
    if (argc > 4 && !strcmp(argv[1], "watch")) {
        int total = atoi(argv[2]), period = atoi(argv[3]), ne = argc - 4 < MAXE ? argc - 4 : MAXE;
        long uid[MAXE]; for (int k = 0; k < ne; ++k) uid[k] = atol(argv[4 + k]);
        LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        float hp0 = -1; static char cur[4096]; timeBeginPeriod(1);
        for (;;) {
            QueryPerformanceCounter(&t); double ms = (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            if (ms > total) break;
            DWORD L = rd32(0xE7F094), pd = rd32(0xFADB58);
            float lx = rdf(L + 0x20), ly = rdf(L + 0x24), lz = rdf(L + 0x28), hp = pd ? rdf(pd + 0x628) : -1;
            if (hp0 >= 0 && hp < hp0 - 0.5f) printf("%.1f hurt %.1f\n", ms, hp0 - hp);
            hp0 = hp;
            if (pd && hp < 600) { float full = 1000; WriteProcessMemory(h, (void*)(size_t)(pd + 0x628), &full, 4, 0); hp0 = full; printf("%.1f heal\n", ms); }
            printf("%.1f lara %.1f %.1f %.1f hp %.0f\n", ms, lx, ly, lz, hp);
            for (int k = 0; k < ne; ++k) {
                DWORD i = find(uid[k]);
                if (!i) { printf("%.1f e %ld none\n", ms, uid[k]); continue; }
                float x = rdf(i + 0x20), y = rdf(i + 0x24), z = rdf(i + 0x28);
                // Lara's sense slot: AI+0xC38 -> EnemySense (0x659BD0 stores it at manager+0xAC8, manager = AI+0x170); flags +0x84
                DWORD c = rd32(i + 0x2D0), ai = c ? rd32(rd32(c) + 0x2EC) : 0, se = ai ? rd32(ai + 0xC38) : 0;
                printf("%.1f e %ld %.1f %.1f %.1f rz %.3f d %.1f sense %lx\n", ms, uid[k], x, y, z, rdf(i + 0x38),
                       sqrtf((x - lx) * (x - lx) + (y - ly) * (y - ly) + (z - lz) * (z - lz)), se && rd32(se + 0xA8) == L ? rd32(se + 0x84) : 0xFFFFFFFFul);
                states(i, cur, sizeof cur);
                if (strcmp(cur, last[k])) { printf("%.1f s %ld%s\n", ms, uid[k], cur); strcpy(last[k], cur); }
            }
            fflush(stdout);
            Sleep(period);
        }
        timeEndPeriod(1);
        return 0;
    }
    puts("usage: enemy list | enemy watch <ms> <period> uid... | enemy zones uid...");
    return 2;
}
