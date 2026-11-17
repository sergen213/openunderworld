// scriptnat_reader.exe: runtime capture of tru.exe's SCMD native binding table (docs/re/SCRIPT_NATIVES.md).
// Run under the same Wine prefix as a running tru.exe (window class TOMBRAIDER8). It scans the process's committed
// private memory for cdc::NativeScriptType objects (vftable in the image, +8 -> "game/script/..." package string,
// +0xC -> native name), follows +0x1C -> the linked cdc::ScriptType, +8 -> its ScriptTypeStreamData, +0x38 -> the
// functions SArray (count at -4, 0x1C bytes each) and prints every function's +0x14 native handler:
//   T <name> <object> <vftable> <scripttype> <function count>
//   F <name> <index> <handler>
// With "code w0 w1 ..." arguments it also lists every place the given consecutive bytecode words occur and the
// 6 words after them (used for the fire volume CALLU operand: code 08000004 09000008 10000010 65000000).
// Build: i686-w64-mingw32-gcc -O2 -o scriptnat_reader.exe tools/re/scriptnat_reader.c
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static HANDLE h;
static DWORD rd32(DWORD a) { DWORD v = 0; ReadProcessMemory(h, (void*)a, &v, 4, 0); return v; }
static int rdstr(DWORD a, char* out, int n) {
    memset(out, 0, n);
    if (!a || !ReadProcessMemory(h, (void*)a, out, n - 1, 0)) return 0;
    for (int i = 0; out[i]; ++i) if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) return 0;
    return out[0] != 0;
}
static int image(DWORD a) { return a >= 0x401000 && a < 0x1000000; }

int main(int argc, char** argv) {
    HWND w = FindWindowA("TOMBRAIDER8", NULL);
    if (!w) { puts("no window"); return 1; }
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { puts("no process"); return 1; }
    DWORD pat[16]; int np = 0;
    if (argc > 2 && !strcmp(argv[1], "code"))
        for (int i = 2; i < argc && np < 16; ++i) pat[np++] = strtoul(argv[i], 0, 16);
    MEMORY_BASIC_INFORMATION mi;
    static unsigned char buf[1 << 20];
    int types = 0, fns = 0;
    // cdc::NativeBuiltInImpl (*0xCC4F68, built by 0x57BF90): +8 MAP (DataType 8) and +0xC DYNARRAY (7) builtin
    // Function arrays (0x1C each, count at -4) that 0x57EE20 links CALLU operands {u8 1, u8 type, u16 index} to.
    DWORD bi = rd32(0xCC4F68);
    for (int k = 0; bi && k < 2; ++k) {
        DWORD fa = rd32(bi + 8 + 4 * k), cnt = fa ? rd32(fa - 4) : 0;
        for (DWORD j = 0; j < cnt && j < 16; ++j)
            printf("B\t%s\t%lu\t%08lx\t%08lx\n", k ? "dynarray" : "map", j, fa + 0x1C * j, rd32(fa + 0x1C * j + 0x14));
    }
    for (DWORD a = 0x10000; a < 0x7fff0000 && VirtualQueryEx(h, (void*)a, &mi, sizeof mi); a = (DWORD)mi.BaseAddress + mi.RegionSize) {
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & 0xEE)) continue;
        DWORD base = (DWORD)mi.BaseAddress, size = mi.RegionSize;
        for (DWORD off = 0; off < size; off += sizeof buf) {
            DWORD n = size - off < sizeof buf ? size - off : sizeof buf;
            SIZE_T got = 0;
            if (!ReadProcessMemory(h, (void*)(base + off), buf, n, &got) || got < 64) continue;
            for (DWORD i = 0; i + 0x24 <= got; i += 4) {
                DWORD* p = (DWORD*)(buf + i);
                if (np && i + 4 * (np + 6) <= got) {
                    int m = 1;
                    for (int k = 0; k < np && m; ++k) m = p[k] == pat[k];
                    if (m) {
                        printf("P %08lx", base + off + i);
                        for (int k = 0; k < np + 6; ++k) printf(" %08lx", p[k]);
                        // the word after the pattern as a Function record: its +0x14 native handler
                        printf(" | next-word handler %08lx\n", rd32(p[np] + 0x14));
                    }
                }
                if (mi.Type != MEM_PRIVATE || !image(p[0]) || !image(p[2]) || !image(p[3])) continue;
                char pkg[16], name[64];
                if (!rdstr(p[2], pkg, sizeof pkg) || strncmp(pkg, "game/script/", 12) || !rdstr(p[3], name, sizeof name)) continue;
                DWORD st = p[7], data = st ? rd32(st + 8) : 0, fa = data ? rd32(data + 0x38) : 0, cnt = fa ? rd32(fa - 4) : 0;
                printf("T %s %08lx %08lx %08lx %lu\n", name, base + off + i, p[0], st, cnt);
                ++types;
                for (DWORD k = 0; k < cnt && k < 512; ++k, ++fns) printf("F\t%s\t%lu\t%08lx\n", name, k, rd32(fa + 0x1C * k + 0x14));
            }
        }
    }
    fprintf(stderr, "%d native types, %d functions\n", types, fns);
    return 0;
}
