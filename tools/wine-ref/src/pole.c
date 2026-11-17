// pole.exe: inspect / drive instances in tru.exe.
//  pole dump uid...            pos (+0x20), rot (+0x30), +0x40 scale?, raw words of each instance
//  pole post uid msg datauid   INSTANCE_Post(0x5B3750)(inst(uid), msg, inst(datauid)) on a remote thread
//  pole postv uid msg value    same with a literal data value
//  pole watch n ms uid...      dump pos/rot n times
//  pole lara x y z             teleport Lara (+0x20)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static HANDLE h;
static DWORD rd32(DWORD a) { DWORD v = 0; ReadProcessMemory(h, (void*)(size_t)a, &v, 4, 0); return v; }
static float rdf(DWORD a) { float v = 0; ReadProcessMemory(h, (void*)(size_t)a, &v, 4, 0); return v; }
static DWORD find(long uid) {
    DWORD i = rd32(0xD98D54); int g = 0;
    while (i && g++ < 6000) { if ((long)rd32(i + 0x58) == uid) return i; i = rd32(i + 8); }
    return 0;
}
static void dump(long uid, int raw) {
    DWORD i = find(uid);
    if (!i) { printf("%ld none\n", uid); return; }
    DWORD obj = rd32(i + 0x10), np = obj ? rd32(obj + 0x60) : 0; char nm[48] = {0};
    if (np) ReadProcessMemory(h, (void*)np, nm, 47, 0);
    printf("%ld %08lx %s pos %.2f %.2f %.2f rot %.4f %.4f %.4f\n", uid, i, nm, rdf(i + 0x20), rdf(i + 0x24), rdf(i + 0x28),
           rdf(i + 0x30), rdf(i + 0x34), rdf(i + 0x38));
    if (raw) {
        for (int k = 0; k < 0x300; k += 4) { printf(" %03x:%08lx", k, rd32(i + k)); if ((k & 0x1c) == 0x1c) printf("\n"); }
        printf("\n");
    }
}
int main(int argc, char** argv) {
    HWND w = FindWindowA("TOMBRAIDER8", NULL);
    if (!w) { puts("no window"); return 1; }
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (argc > 2 && !strcmp(argv[1], "dump")) { for (int k = 2; k < argc; ++k) dump(atol(argv[k]), 0); return 0; }
    if (argc > 2 && !strcmp(argv[1], "raw")) { for (int k = 2; k < argc; ++k) dump(atol(argv[k]), 1); return 0; }
    if (argc > 4 && !strcmp(argv[1], "watch")) {
        int n = atoi(argv[2]), ms = atoi(argv[3]);
        for (int t = 0; t < n; ++t) { printf("t %d\n", t); for (int k = 4; k < argc; ++k) dump(atol(argv[k]), 0); fflush(stdout); Sleep(ms); }
        return 0;
    }
    if (argc > 4 && (!strcmp(argv[1], "post") || !strcmp(argv[1], "postv"))) {
        DWORD inst = find(atol(argv[2])), msg = strtoul(argv[3], 0, 16);
        DWORD data = !strcmp(argv[1], "post") ? find(atol(argv[4])) : strtoul(argv[4], 0, 16);
        if (!inst) { puts("no instance"); return 1; }
        unsigned char c[64]; int n = 0;
#define B(x) c[n++] = (unsigned char)(x)
#define D(x) do { DWORD v_ = (x); memcpy(c + n, &v_, 4); n += 4; } while (0)
        B(0x68); D(data); B(0x68); D(msg); B(0x68); D(inst); B(0xB8); D(0x5B3750); B(0xFF); B(0xD0); B(0x83); B(0xC4); B(0x0C);
        B(0x31); B(0xC0); B(0xC2); B(0x04); B(0x00);
        void* r = VirtualAllocEx(h, 0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        WriteProcessMemory(h, r, c, n, 0);
        HANDLE t = CreateRemoteThread(h, 0, 0, (LPTHREAD_START_ROUTINE)r, 0, 0, 0);
        WaitForSingleObject(t, 5000);
        printf("posted %lx to %08lx data %08lx\n", msg, inst, data);
        return 0;
    }
    if (argc > 4 && !strcmp(argv[1], "lara")) {
        DWORD l = rd32(0xE7F094); float p[3] = {(float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4])};
        WriteProcessMemory(h, (void*)(l + 0x20), p, 12, 0);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "yaw")) {
        DWORD l = rd32(0xE7F094); float y = (float)atof(argv[2]);
        WriteProcessMemory(h, (void*)(l + 0x38), &y, 4, 0);
        return 0;
    }
    return 2;
}
