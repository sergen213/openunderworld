// chart.exe: per-frame trace of Lara's player state chart in tru.exe, in lockstep with the game (docs/re/PLAYER_CHART.md,
// tools/wine-ref/README.md "Chart traces"). Subcommands:
//   chart info                      chart object, header, current state / parent / request / transition
//   chart hook | unhook             install / remove the lockstep detour at the frame-counter increment (0x5E003F)
//   chart trace <ticks> [vk:down:up ...]   lockstep trace; key times are TICKS (game frames) from the trace start
//
// Chart runtime [O: 0x787570 FindTransition, 0x7874D0 / 0x787E00 reset / enter, the ChangeState body at 0x789040..,
// 0x785920 transition conditions, 0x783470 state anim request]: the locomotion chart object is *(playerData 0xFADB58 +
// 0xF10) (the upper-body chart +0xF14); chart +4 -> the chart header in lara.drm (s16 +0 initial state, s16 +2 agent id,
// u16 +8 states, u16 +0xA transitions, +0xC state pointer table, +0x10 transition pointer table); chart +0x510 the
// current state record (0x28 bytes: s16 +4 anim request, s16 +6 parent), +0x514 the queued transition, +0x518 the
// transition taken last (pointer into the transition records, 0x14 bytes, s16 +0 target state), +0x50C the chart's
// update stamp (condition caches compare it), +0x51C cleared on every state change.
// Frame [O: GAMELOOP_Frame 0x5DFFE0]: ... GAMELOOP_UpdateTime 0x5DA230; gameTracker (0xE7F088) +0x24 += 1 at 0x5E003F
// (`add dword [esi+0x24],1; pop esi; ret`, 6 bytes). The detour replaces those bytes with a jump to a cave that does
// the add, publishes the new frame number and spins (pause loop, ~5 s cap, after which it disables itself) until the
// tracer acknowledges it. So the tracer sees every frame exactly once, between the frame's UpdateTime and the next
// frame's simulation, and its key presses land between two known frames.
// Fixed step [O: TIMING.md, 0x5DA230]: gameTracker +0xC8 variableTimestep = 0 with frameRateLock +0x120 = 3 makes every
// frame a 16 667 us step (timeMult 0.5): the trace is then a 60 Hz tick sequence independent of the render rate.
// trace writes both while it runs (FIXED=0: leave the variable step) and restores the variable step at the end.
//
// Output (contract TSV, W/chart/wine/README.md): one header line, then one row per frame:
//   tick state parent request stateTicks px py pz vx vy vz yaw inputs [var_<name>=value ...]
//   tick = frame - first frame (fixed 60 Hz steps); state = index into the chart's state table (S###); request /
//   parent from the state record; stateTicks = frames since the state was entered, 0 on the entry tick (a transition
//   back into the same state counts as an entry); p = Instance +0x20 minus OFFSET (OFFSET=dx,dy,dz, tru = ours + d:
//   our unit frame), v = world velocity in units/s (Instance +0x2A0 is local: x left, y -forward; x 30), yaw =
//   Instance +0x38 - 90 deg in degrees (our frame), var_lvx / var_lvy the raw local +0x2A0 x / y (units per 30 Hz frame);
//   inputs = hex mask of the keys held while this frame was simulated (1 Fwd 2 Back 4 Left 8 Right 0x10 Jump 0x20
//   Interact 0x40 Crouch 0x80 Sprint 0x100 Walk 0x200 Grapple). Extra columns: t30 (chart +0x508, state time in 30 Hz
//   frames, as ChartCore's), var_frame (gameTracker +0x24), var_tr (index of the last transition taken, +0x518), var_q
//   (queued transition +0x514, -1 none), var_ub (upper-body chart state), var_an (Instance +0xC18 anim word),
//   var_tpx/tpy/tpz/tyaw (raw tru position / yaw in radians), var_ms (wall ms), and VARS=name:base:hexoff[:f],...
//   (base pd = player data, p = instance, c = chart object; :f prints the dword as a float).
//   SCANAT=tick SCANV=x,y (debug): list where instance / proxy / player data hold the float pair (x, y) at that tick.
// Env: SPAWN=x,y,z[,yaw] (written with zero velocity while paused for HOLD ticks, default 20, before tick 0),
//   CAMFOLLOW=dx,dy,dist,h (camera pinned behind her every frame: Forward means world (dx,dy)), GOD (default 1: health
//   kept full), KEYWAIT=ms (default 40: the frame is held this long after a key change so that the press is queued
//   before the next frame's message pump), FIXED (default 1).
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GT 0xE7F088u
#define HOOK 0x5E003Fu
static HANDLE h;
static DWORD rd32(DWORD a) { DWORD v = 0; ReadProcessMemory(h, (void*)a, &v, 4, 0); return v; }
static short rd16(DWORD a) { short v = 0; ReadProcessMemory(h, (void*)a, &v, 2, 0); return v; }
static float rdf(DWORD a) { float v = 0; ReadProcessMemory(h, (void*)a, &v, 4, 0); return v; }
static void w32(DWORD a, DWORD v) { WriteProcessMemory(h, (void*)a, &v, 4, 0); }

static int patchSuspended(DWORD a, const unsigned char* v, int n) { // as wmem wbs: no thread inside [a, a+n)
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

static const unsigned char kOrig[6] = {0x83, 0x46, 0x24, 0x01, 0x5E, 0xC3};
// Cave data: +0x100 enabled, +0x104 posted frame, +0x108 acknowledged frame.
static DWORD caveOf(void) {
    unsigned char b[6]; ReadProcessMemory(h, (void*)HOOK, b, 6, 0);
    if (b[0] != 0xE9) return 0;
    DWORD rel; memcpy(&rel, b + 1, 4); return HOOK + 5 + rel;
}
static DWORD hook(void) {
    DWORD c = caveOf();
    if (c) return c;
    unsigned char b[6]; ReadProcessMemory(h, (void*)HOOK, b, 6, 0);
    if (memcmp(b, kOrig, 6)) { puts("unexpected bytes at 0x5E003F"); exit(1); }
    c = (DWORD)(size_t)VirtualAllocEx(h, 0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    DWORD en = c + 0x100, po = c + 0x104, ak = c + 0x108;
    unsigned char s[96]; int n = 0;
#define B(x) s[n++] = (unsigned char)(x)
#define D(x) do { DWORD v_ = (x); memcpy(s + n, &v_, 4); n += 4; } while (0)
    B(0x83); B(0x46); B(0x24); B(0x01);              // add dword [esi+0x24],1
    B(0x83); B(0x3D); D(en); B(0x00);                // cmp dword [en],0
    B(0x74); int jDone = n; B(0);                    // je done
    B(0x50); B(0x51);                                // push eax; push ecx
    B(0x8B); B(0x46); B(0x24);                       // mov eax,[esi+0x24]
    B(0xA3); D(po);                                  // mov [po],eax
    B(0xB9); D(0x10000000);                          // mov ecx,0x10000000
    int L = n;
    B(0x39); B(0x05); D(ak);                         // cmp [ak],eax
    B(0x74); int jOut = n; B(0);                     // je out
    B(0xF3); B(0x90);                                // pause
    B(0x49);                                         // dec ecx
    B(0x75); { int rel8 = L - (n + 1); B(rel8); }     // jnz L
    B(0xC7); B(0x05); D(en); D(0);                   // mov dword [en],0 (tracer gone: stop waiting)
    s[jOut] = (unsigned char)(n - (jOut + 1));
    B(0x59); B(0x58);                                // out: pop ecx; pop eax
    s[jDone] = (unsigned char)(n - (jDone + 1));
    B(0x5E); B(0xC3);                                // done: pop esi; ret
    WriteProcessMemory(h, (void*)(size_t)c, s, n, 0);
    w32(en, 0); w32(po, 0); w32(ak, 0);
    unsigned char j[6] = {0xE9, 0, 0, 0, 0, 0x90}; DWORD rel = c - (HOOK + 5); memcpy(j + 1, &rel, 4);
    if (!patchSuspended(HOOK, j, 6)) { puts("patch failed"); exit(1); }
    return c;
}

// Call log (CALLS=1, `chart callhook` / `callunhook`): detours at the entry of a list of functions, by default the anim
// functions 0x441FB0 (start a clip on a channel, with its crossfade), 0x432850 (set a channel's clip time) and 0x4428A0
// (a layer's blend test); CALLFN=hex[:k[:k2]][:Rn],... replaces the list (up to 8; each must begin with the 6-byte
// prologue below): `:k` (1..5, up to two) also copies the 4 dwords argument k points to at entry, `:Rn` copies the 8
// dwords argument n points to when the call returns (the entry swaps the return address for an exit stub; not
// reentrant: a nested call of the same function loses the outer result). Each entry records the function, ECX, the
// first five stack arguments, the frame number, the return address and the copies into a 256-entry ring (128-byte
// slots) in the cave; the tracer prints the new entries of each tick as
// `var_calls=fn:ecx:a1:a2:a3:a4:a5[:d0:d1:d2:d3]...^ret[=r0:..:r7]@frame;...` (dwords in hex; floats are their bit
// patterns; `=?` a call that had not returned). All begin with `push ebp; mov ebp,esp; and esp,-16` (6 bytes), which
// the cave replays before jumping back. Per function a 0x200 block (entry +0, exit stub +0x100, vars +0x1F0); the
// cave keeps the list (+0x1E00 count, +0x1E04 functions, +0x1E24 / +0x1E44 derefs, +0x1E64 result arg) for callunhook.
static DWORD gFn[8] = {0x441FB0, 0x432850, 0x4428A0}; static int gDeref[8][2], gRes[8], gN = 3;
static const unsigned char kProlog[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
static void callList(void) {
    if (!getenv("CALLFN")) return;
    char buf[256]; strncpy(buf, getenv("CALLFN"), 255); buf[255] = 0; gN = 0;
    for (char* t = strtok(buf, ","); t && gN < 8; t = strtok(0, ",")) {
        gFn[gN] = (DWORD)strtoul(t, 0, 16); gDeref[gN][0] = gDeref[gN][1] = gRes[gN] = 0; int nd = 0;
        for (char* q = strchr(t, ':'); q; q = strchr(q + 1, ':')) {
            if (q[1] == 'R') { int k = atoi(q + 2); if (k >= 1 && k <= 5) gRes[gN] = k; }
            else { int k = atoi(q + 1); if (k >= 1 && k <= 5 && nd < 2) gDeref[gN][nd++] = k; } }
        ++gN; }
}
static DWORD callCaveOf(void) { // the ring's cave: from the first function's jump (cave code block 0 at +0)
    unsigned char b[6]; ReadProcessMemory(h, (void*)gFn[0], b, 6, 0);
    if (b[0] != 0xE9) return 0;
    DWORD rel; memcpy(&rel, b + 1, 4); return gFn[0] + 5 + rel;
}
static DWORD callhook(void) {
    DWORD c = callCaveOf();
    if (c) return c;
    for (int f = 0; f < gN; ++f) { unsigned char b[6]; ReadProcessMemory(h, (void*)gFn[f], b, 6, 0);
        if (memcmp(b, kProlog, 6)) { printf("unexpected bytes at %08lx\n", gFn[f]); exit(1); } }
    c = (DWORD)(size_t)VirtualAllocEx(h, 0, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    DWORD wpos = c + 0x1F00, ring = c + 0x2000;
    w32(wpos, 0); w32(c + 0x1E00, gN);
    for (int f = 0; f < gN; ++f) { w32(c + 0x1E04 + 4 * f, gFn[f]); w32(c + 0x1E24 + 4 * f, gDeref[f][0]);
        w32(c + 0x1E44 + 4 * f, gDeref[f][1]); w32(c + 0x1E64 + 4 * f, gRes[f]); }
    for (int f = 0; f < gN; ++f) {
        unsigned char s[512]; int n = 0; DWORD at = c + 0x200 * f, ex = at + 0x100, vRet = at + 0x1F0, vSlot = at + 0x1F4, vRes = at + 0x1F8;
        B(0x50); B(0x52); B(0x51);                         // push eax; push edx; push ecx
        B(0xA1); D(wpos);                                  // mov eax,[wpos]
        B(0x25); D(0xFF);                                  // and eax,255
        B(0xC1); B(0xE0); B(7);                            // shl eax,7
        B(0x05); D(ring);                                  // add eax,ring
        B(0xC7); B(0x00); D(gFn[f]);                       // mov dword [eax],fn
        B(0x89); B(0x48); B(4);                            // mov [eax+4],ecx
        for (int k = 0; k < 5; ++k) {                      // args: [esp+16+4k] (3 pushes + return address)
            B(0x8B); B(0x54); B(0x24); B(16 + 4 * k);      // mov edx,[esp+16+4k]
            B(0x89); B(0x50); B(8 + 4 * k);                // mov [eax+8+4k],edx
        }
        B(0x8B); B(0x15); D(GT + 0x24);                    // mov edx,[gameTracker+0x24]
        B(0x89); B(0x50); B(28);                           // mov [eax+28],edx
        B(0x8B); B(0x54); B(0x24); B(12);                  // mov edx,[esp+12] (return address)
        B(0x89); B(0x50); B(32);                           // mov [eax+32],edx
        for (int d = 0; d < 2; ++d) if (gDeref[f][d]) {
            B(0x8B); B(0x54); B(0x24); B(16 + 4 * (gDeref[f][d] - 1)); // mov edx,[esp+16+4(k-1)] (the pointer)
            for (int j = 0; j < 4; ++j) { B(0x8B); B(0x4A); B(4 * j); B(0x89); B(0x48); B(36 + 16 * d + 4 * j); } // mov ecx,[edx+4j]; mov [eax+36+16d+4j],ecx
        }
        if (gRes[f]) {
            B(0xC7); B(0x40); B(100); D(0);                // mov dword [eax+100],0 (not returned yet)
            B(0xA3); D(vSlot);                             // mov [vSlot],eax
            B(0x8B); B(0x54); B(0x24); B(16 + 4 * (gRes[f] - 1)); B(0x89); B(0x15); D(vRes); // mov edx,[esp+16+4(n-1)]; mov [vRes],edx
            B(0x8B); B(0x54); B(0x24); B(12); B(0x89); B(0x15); D(vRet); // mov edx,[esp+12]; mov [vRet],edx
            B(0xC7); B(0x44); B(0x24); B(12); D(ex);       // mov dword [esp+12],exit stub
        }
        B(0xFF); B(0x05); D(wpos);                         // inc dword [wpos]
        B(0x59); B(0x5A); B(0x58);                         // pop ecx; pop edx; pop eax
        memcpy(s + n, kProlog, 6); n += 6;                 // the replaced prologue
        B(0xE9); D(gFn[f] + 6 - (at + n + 4));             // jmp fn+6
        WriteProcessMemory(h, (void*)(size_t)at, s, n, 0);
        if (gRes[f]) { // exit stub: copy 8 result dwords into the slot, flag it, return to the saved address
            n = 0;
            B(0x50); B(0x52); B(0x51);                     // push eax; push edx; push ecx
            B(0xA1); D(vSlot);                             // mov eax,[vSlot]
            B(0x8B); B(0x15); D(vRes);                     // mov edx,[vRes]
            for (int j = 0; j < 8; ++j) { B(0x8B); B(0x4A); B(4 * j); B(0x89); B(0x48); B(68 + 4 * j); } // mov ecx,[edx+4j]; mov [eax+68+4j],ecx
            B(0xC7); B(0x40); B(100); D(1);                // mov dword [eax+100],1
            B(0x59); B(0x5A); B(0x58);                     // pop ecx; pop edx; pop eax
            B(0xFF); B(0x25); D(vRet);                     // jmp [vRet]
            WriteProcessMemory(h, (void*)(size_t)ex, s, n, 0);
        }
    }
    for (int f = gN - 1; f >= 0; --f) { // the first function last: callCaveOf finds the cave by it
        unsigned char j[6] = {0xE9, 0, 0, 0, 0, 0x90}; DWORD rel = (c + 0x200 * f) - (gFn[f] + 5); memcpy(j + 1, &rel, 4);
        if (!patchSuspended(gFn[f], j, 6)) { puts("call patch failed"); exit(1); }
    }
    return c;
}

// Ray log (RAY=1, `chart rayhook` / `rayunhook`): tru.exe's wall-under-hang ray 0x744E90(matrix*, float, arg3) (called
// by 0x74F480; it builds two points from the matrix and player tuning +0x6E4 / +0x6E6 and casts 0x4935F0 with ray
// flags 0x2C19E7, returning result+0x10 != 0). Entry detour (the 6-byte prologue as in callhook): tag 1, frame, the
// three arguments and the matrix's 16 dwords. Exit detour at 0x744FBB (`add esp,0x24; xor eax,eax`, after the ray
// call, result struct at esp+0x40): tag 2, frame, the result's 24 dwords (0x4935F0: +0x10 kind, 3 = hit, 0 = none;
// +0..+8 / +0xC, +0x18, +0x40.. +0x58 the hit's fields). 128-byte slots in a 256-slot ring; printed per tick as
// var_ray=E@frame:a1:a2:a3:m0..m15;R@frame:kind:r0..r23;... (raw hex dwords).
#define RAYEXIT 0x744FBBu
static const unsigned char kRayExit[5] = {0x83, 0xC4, 0x24, 0x33, 0xC0};
static DWORD rayCaveOf(void) {
    unsigned char b[6]; ReadProcessMemory(h, (void*)RAYEXIT, b, 5, 0);
    if (b[0] != 0xE9) return 0;
    DWORD rel; memcpy(&rel, b + 1, 4); return RAYEXIT + 5 + rel - 0x100; // exit code sits at cave + 0x100
}
static DWORD rayhook(void) {
    DWORD c = rayCaveOf();
    if (c) return c;
    unsigned char b[6]; ReadProcessMemory(h, (void*)0x744E90, b, 6, 0);
    if (memcmp(b, kProlog, 6)) { puts("unexpected bytes at 744e90"); exit(1); }
    ReadProcessMemory(h, (void*)RAYEXIT, b, 5, 0);
    if (memcmp(b, kRayExit, 5)) { puts("unexpected bytes at 744fbb"); exit(1); }
    c = (DWORD)(size_t)VirtualAllocEx(h, 0, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    DWORD wpos = c + 0xF00, ring = c + 0x1000;
    w32(wpos, 0);
    for (int f = 0; f < 2; ++f) {
        unsigned char s[512]; int n = 0; DWORD at = c + 0x100 * f;
        if (f == 1) { B(0x83); B(0xC4); B(0x24); }        // add esp,0x24 (replaced)
        B(0x50); B(0x51); B(0x52);                         // push eax; push ecx; push edx
        B(0xA1); D(wpos);                                  // mov eax,[wpos]
        B(0x25); D(0xFF);                                  // and eax,255
        B(0xC1); B(0xE0); B(7);                            // shl eax,7
        B(0x05); D(ring);                                  // add eax,ring
        B(0xC7); B(0x00); D(f + 1);                        // mov dword [eax],tag
        B(0x8B); B(0x15); D(GT + 0x24);                    // mov edx,[frame]
        B(0x89); B(0x50); B(4);                            // mov [eax+4],edx
        if (f == 0) {
            B(0x8B); B(0x54); B(0x24); B(16);              // mov edx,[esp+16] (a1)
            B(0x89); B(0x50); B(8);
            B(0x8B); B(0x4C); B(0x24); B(20); B(0x89); B(0x48); B(12);   // a2
            B(0x8B); B(0x4C); B(0x24); B(24); B(0x89); B(0x48); B(16);   // a3
            for (int k = 0; k < 16; ++k) { B(0x8B); B(0x4A); B(4 * k); B(0x89); B(0x48); B(20 + 4 * k); } // matrix
        } else {
            for (int k = 0; k < 24; ++k) { B(0x8B); B(0x8C); B(0x24); D(0x4C + 4 * k); B(0x89); B(0x48); B(8 + 4 * k); }
        }
        B(0xFF); B(0x05); D(wpos);                         // inc dword [wpos]
        B(0x5A); B(0x59); B(0x58);                         // pop edx; pop ecx; pop eax
        if (f == 0) { memcpy(s + n, kProlog, 6); n += 6; B(0xE9); D(0x744E96 - (at + n + 4)); }
        else { B(0x33); B(0xC0); B(0xE9); D(0x744FC0 - (at + n + 4)); } // xor eax,eax (replaced); jmp on
        WriteProcessMemory(h, (void*)(size_t)at, s, n, 0);
    }
    unsigned char j[6] = {0xE9, 0, 0, 0, 0, 0x90}; DWORD rel = (c + 0x100) - (RAYEXIT + 5); memcpy(j + 1, &rel, 4);
    if (!patchSuspended(RAYEXIT, j, 5)) { puts("ray patch failed"); exit(1); }
    rel = c - (0x744E90 + 5); memcpy(j + 1, &rel, 4);
    if (!patchSuspended(0x744E90, j, 6)) { puts("ray patch failed"); exit(1); }
    return c;
}

// Index of a pointer in a table of n pointers (state or transition table), -1 if absent.
static DWORD tab[4][4096]; static DWORD tabAt[4]; static int tabN[4];
static int indexIn(int slot, DWORD table, int n, DWORD ptr) {
    if (!table || !ptr) return -1;
    if (n > 4096) n = 4096;
    if (tabAt[slot] != table || tabN[slot] != n) {
        memset(tab[slot], 0, sizeof tab[slot]); ReadProcessMemory(h, (void*)table, tab[slot], n * 4, 0);
        tabAt[slot] = table; tabN[slot] = n;
    }
    for (int i = 0; i < n; ++i) if (tab[slot][i] == ptr) return i;
    return -1;
}
typedef struct { DWORD chart, hdr, st, tr, q; int idx, trIdx, qIdx, req, par; } Chart;
static Chart readChart(DWORD chart, int slot) {
    Chart c = {0}; c.chart = chart; c.idx = c.trIdx = c.qIdx = c.req = c.par = -1;
    if (!chart) return c;
    c.hdr = rd32(chart + 4); c.st = rd32(chart + 0x510); c.q = rd32(chart + 0x514); c.tr = rd32(chart + 0x518);
    if (!c.hdr) return c;
    int ns = (unsigned short)rd16(c.hdr + 8), nt = (unsigned short)rd16(c.hdr + 0xA);
    c.idx = indexIn(slot * 2, rd32(c.hdr + 0xC), ns, c.st);
    c.trIdx = indexIn(slot * 2 + 1, rd32(c.hdr + 0x10), nt, c.tr);
    c.qIdx = c.q ? indexIn(slot * 2 + 1, rd32(c.hdr + 0x10), nt, c.q) : -1;
    if (c.st) { c.req = rd16(c.st + 4); c.par = rd16(c.st + 6); }
    return c;
}

static void sendKey(int vk, int up) { // as ref.c: 1 / 2 mouse buttons, else a scan-code key (arrows extended)
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
static unsigned maskOf(int vk) {
    switch (vk) {
    case 0x26: case 0x57: return 0x1;    // Up / W: forward
    case 0x28: case 0x53: return 0x2;    // Down / S
    case 0x25: case 0x41: return 0x4;    // Left / A
    case 0x27: case 0x44: return 0x8;    // Right / D
    case 0x20: return 0x10;              // Space: jump
    case 0x45: return 0x20;              // E: interact
    case 0x11: case 0xA2: return 0x40;   // Ctrl: crouch / drop
    case 0xA0: case 0x10: return 0x80;   // LShift: sprint
    case 0xA4: case 0x12: return 0x100;  // LAlt: walk
    case 0x51: return 0x200;             // Q: grapple
    }
    return 0;
}

typedef struct { char name[24]; char base; DWORD off; int isF; } Var;

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    callList();
    HWND w = FindWindowA("TOMBRAIDER8", NULL);
    if (!w) { puts("no window"); return 1; }
    DWORD pid; GetWindowThreadProcessId(w, &pid);
    h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) { puts("no process"); return 1; }
    if (!strcmp(argv[1], "info")) {
        DWORD pd = rd32(0xFADB58), p = rd32(0xE7F094);
        for (int k = 0; k < 2; ++k) {
            DWORD ch = pd ? rd32(pd + 0xF10 + 4 * k) : 0; Chart c = readChart(ch, k);
            printf("chart%d %08lx hdr %08lx init %d agent %d states %u trans %u stab %08lx ttab %08lx | state %08lx S%d req %d par %d"
                   " tr %d q %d stamp %lu 51c %08lx\n", k, ch, c.hdr, c.hdr ? rd16(c.hdr) : -1, c.hdr ? rd16(c.hdr + 2) : -1,
                   c.hdr ? (unsigned short)rd16(c.hdr + 8) : 0, c.hdr ? (unsigned short)rd16(c.hdr + 0xA) : 0,
                   c.hdr ? rd32(c.hdr + 0xC) : 0, c.hdr ? rd32(c.hdr + 0x10) : 0, c.st, c.idx, c.req, c.par, c.trIdx, c.qIdx,
                   ch ? rd32(ch + 0x50C) : 0, ch ? rd32(ch + 0x51C) : 0);
        }
        printf("frame %lu varstep %lu lock %lu pos %.2f %.2f %.2f yaw %.4f hook %08lx\n", rd32(GT + 0x24), rd32(GT + 0xC8),
               rd32(GT + 0x120), rdf(p + 0x20), rdf(p + 0x24), rdf(p + 0x28), rdf(p + 0x38), caveOf());
        return 0;
    }
    if (!strcmp(argv[1], "hook")) { printf("cave %08lx\n", hook()); return 0; }
    if (!strcmp(argv[1], "callhook")) { printf("call cave %08lx\n", callhook()); return 0; }
    if (!strcmp(argv[1], "rayhook")) { printf("ray cave %08lx\n", rayhook()); return 0; }
    if (!strcmp(argv[1], "rayunhook")) {
        if (!rayCaveOf()) { puts("not hooked"); return 0; }
        int ok = patchSuspended(0x744E90, kProlog, 6) & patchSuspended(RAYEXIT, kRayExit, 5);
        puts(ok ? "unhooked" : "unhook failed"); return !ok;
    }
    if (!strcmp(argv[1], "callunhook")) {
        DWORD c = callCaveOf(); if (!c) { puts("not hooked"); return 0; }
        int ok = 1, n = (int)rd32(c + 0x1E00); for (int f = 0; f < n && f < 8; ++f) ok &= patchSuspended(rd32(c + 0x1E04 + 4 * f), kProlog, 6);
        puts(ok ? "unhooked" : "unhook failed"); return !ok;
    }
    if (!strcmp(argv[1], "unhook")) {
        DWORD c = caveOf(); if (!c) { puts("not hooked"); return 0; }
        w32(c + 0x100, 0); w32(c + 0x108, rd32(c + 0x104));
        int ok = patchSuspended(HOOK, kOrig, 6); puts(ok ? "unhooked" : "unhook failed"); return !ok;
    }
    if (strcmp(argv[1], "trace") || argc < 3) return 2;

    int ticks = atoi(argv[2]), ne = argc - 3; if (ne > 64) ne = 64;
    WORD vk[64]; int dn[64], up[64], st[64] = {0};
    for (int i = 0; i < ne; ++i) { unsigned k; sscanf(argv[3 + i], "%x:%d:%d", &k, &dn[i], &up[i]); vk[i] = (WORD)k; }
    int hold = getenv("HOLD") ? atoi(getenv("HOLD")) : 20, keyWait = getenv("KEYWAIT") ? atoi(getenv("KEYWAIT")) : 40;
    int god = !getenv("GOD") || atoi(getenv("GOD")), fixed = !getenv("FIXED") || atoi(getenv("FIXED"));
    float sp[4] = {0}; int nsp = getenv("SPAWN") ? sscanf(getenv("SPAWN"), "%f,%f,%f,%f", sp, sp + 1, sp + 2, sp + 3) : 0;
    float off[3] = {0, 0, 0}; if (getenv("OFFSET")) sscanf(getenv("OFFSET"), "%f,%f,%f", off, off + 1, off + 2);
    if (nsp < 3) hold = 0;
    int settle = getenv("SETTLE") ? atoi(getenv("SETTLE")) : 0; // free, unrecorded frames between the pin and tick 0
    float cf[4]; int cam = getenv("CAMFOLLOW") && sscanf(getenv("CAMFOLLOW"), "%f,%f,%f,%f", cf, cf + 1, cf + 2, cf + 3) == 4;
    // VARS bases: pd player data, p instance, c chart, L Lara's main anim layer *(*(p+0x280)+0x14) (its +0x00..+0x24 is the
    // alignment record 0x43E480 / 0x43E370 write: +0 target position, +0x10 rotation, +0x20 end time, +0x24 mode), r the
    // layer's crossfade record *(L+0x7C) (0x441FB0: +0x50 outgoing root velocity, +0x60 incoming), k channel 0 *(L+0x80),
    // s the stick controller *(*(0xFADFB0)+0xB0), a an absolute address (the offset is the address). Without :f the
    // dword is printed as raw hex (e.g. a float's exact bits).
    Var vars[64]; int nv = 0;
    if (getenv("VARS")) { static char buf[4096]; strncpy(buf, getenv("VARS"), 4095); buf[4095] = 0;
        for (char* t = strtok(buf, ","); t && nv < 64; t = strtok(0, ",")) {
            Var v = {{0}}; char b[8] = {0}, f[4] = {0}; unsigned off;
            if (sscanf(t, "%23[^:]:%7[^:]:%x:%3s", v.name, b, &off, f) >= 3) { v.base = !strcmp(b, "pd") ? 'd' : b[0]; v.off = off; v.isF = f[0] == 'f'; vars[nv++] = v; } } }

    SetForegroundWindow(w); timeBeginPeriod(1);
    DWORD cave = hook(), EN = cave + 0x100, PO = cave + 0x104, AK = cave + 0x108;
    DWORD callCave = (getenv("CALLS") && atoi(getenv("CALLS"))) ? callhook() : 0, callRead = callCave ? rd32(callCave + 0x1F00) : 0;
    DWORD rayCave = (getenv("RAY") && atoi(getenv("RAY"))) ? rayhook() : 0, rayRead = rayCave ? rd32(rayCave + 0xF00) : 0;
    DWORD oldVar = rd32(GT + 0xC8), oldLock = rd32(GT + 0x120);
    if (fixed) { w32(GT + 0x120, 3); w32(GT + 0xC8, 0); }
    // Arm: release a frame a killed tracer left waiting, then the next frame end publishes its number and waits for us.
    w32(EN, 0); w32(AK, rd32(PO)); Sleep(100);
    w32(AK, 0); w32(PO, 0); w32(EN, 1);
    LARGE_INTEGER fq, t0, tn; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    printf("tick\tstate\tparent\trequest\tstateTicks\tpx\tpy\tpz\tvx\tvy\tvz\tyaw\tinputs\n"); fflush(stdout);
    DWORD f0 = 0, lastSt = 0, lastTr = 0; int t = -hold - settle - 1, stTicks = 0; unsigned mask = 0; int lost = 0;
    while (t < ticks) {
        // wait for the next published frame (or the cave giving up)
        DWORD f;
        for (int spin = 0;; ++spin) {
            f = rd32(PO);
            if (f && f != rd32(AK)) break;
            if (!rd32(EN)) { fprintf(stderr, "lockstep lost (cave timed out)\n"); lost = 1; break; }
            if (spin > 20000) { fprintf(stderr, "no frame for 20 s\n"); lost = 1; break; }
            Sleep(0);
            if (spin > 200) Sleep(1);
        }
        if (lost) break;
        ++t;
        if (t == 0) f0 = f;
        DWORD p = rd32(0xE7F094), pd = rd32(0xFADB58), c1 = pd ? rd32(pd + 0xF10) : 0, c2 = pd ? rd32(pd + 0xF14) : 0;
        if (t < -settle) { // holding at the spawn: position, velocity, heading, proxy capsule (+0x288 +0x640/+0x680/+0x6D0)
            float z[3] = {0, 0, 0}, cc[3] = {sp[0], sp[1], sp[2] + 315.4f}; DWORD q = rd32(p + 0x288);
            WriteProcessMemory(h, (void*)(p + 0x20), sp, 12, 0); WriteProcessMemory(h, (void*)(p + 0x2A0), z, 12, 0);
            if (nsp > 3) WriteProcessMemory(h, (void*)(p + 0x38), sp + 3, 4, 0);
            if (q) for (int o = 0; o < 3; ++o) WriteProcessMemory(h, (void*)(q + (o == 0 ? 0x640 : o == 1 ? 0x680 : 0x6D0)), cc, 12, 0);
        }
        if (god && pd) { float hp = 1000.0f; WriteProcessMemory(h, (void*)(pd + 0x628), &hp, 4, 0); }
        if (cam) { DWORD cm = rd32(0xE80534); if (cm && p) { float lp[3]; ReadProcessMemory(h, (void*)(p + 0x20), lp, 12, 0);
            float cp[3] = {lp[0] - cf[0] * cf[2], lp[1] - cf[1] * cf[2], lp[2] + cf[3]};
            WriteProcessMemory(h, (void*)(cm + 0x90), cp, 12, 0); WriteProcessMemory(h, (void*)(cm + 0x150), cp, 12, 0); } }
        if (t >= 0) {
            Chart a = readChart(c1, 0), b = readChart(c2, 1);
            if (a.st != lastSt || (a.tr != lastTr && a.tr && rd16(a.tr) == a.idx)) stTicks = 0; else ++stTicks;
            lastSt = a.st; lastTr = a.tr;
            float s[7] = {0}; if (p) { ReadProcessMemory(h, (void*)(p + 0x20), s, 12, 0); ReadProcessMemory(h, (void*)(p + 0x2A0), s + 3, 12, 0); s[6] = rdf(p + 0x38); }
            unsigned short an = 0; if (p) ReadProcessMemory(h, (void*)(p + 0xC18), &an, 2, 0);
            QueryPerformanceCounter(&tn);
            float t30 = c1 ? rdf(c1 + 0x508) : 0, yawDeg = (s[6] - 1.5707963f) * 57.29578f;
            while (yawDeg > 180) yawDeg -= 360;
            while (yawDeg <= -180) yawDeg += 360;
            // +0x2A0 is kept in her local frame (x = left, y = -forward, z up; checked against the position steps of
            // the running jump, the hang leaps back / aside): world = forward * -y + left * x, our heading psi.
            float ps = (yawDeg / 57.29578f), fx = cosf(ps), fy = sinf(ps);
            float wvx = (-s[4] * fx - s[3] * fy) * 30, wvy = (-s[4] * fy + s[3] * fx) * 30;
            printf("%d\t%d\t%d\t%d\t%d\t%.2f\t%.2f\t%.2f\t%.1f\t%.1f\t%.1f\t%.3f\t%x\tt30=%.3f\tvar_frame=%lu\tvar_tr=%d\tvar_q=%d\tvar_ub=%d\tvar_an=%u"
                   "\tvar_tpx=%.2f\tvar_tpy=%.2f\tvar_tpz=%.2f\tvar_tyaw=%.4f\tvar_lvx=%.2f\tvar_lvy=%.2f\tvar_ms=%.0f",
                   t, a.idx, a.par, a.req, stTicks, s[0] - off[0], s[1] - off[1], s[2] - off[2], wvx, wvy, s[5] * 30,
                   yawDeg, mask, t30, f, a.trIdx, a.qIdx, b.idx, an, s[0], s[1], s[2], s[6], s[3], s[4],
                   (tn.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart);
            // ANIM=1: Lara's main anim layer L = *(*(Instance+0x280)+0x14) (the layer 0x43B390 tests; spec by ChartJ):
            // L +0x44 s32 current pattern, +0xA4 / +0xA8 / +0xAC floats (0x4428A0's blend test), +0x314 s8 next-blend
            // override, +0xF6 u8 channel count, +0xF8 u8 current channel, +0x300 u8 queue busy, +0x2F4 / +0x2F8 s32 queue
            // read / write, +0x80 channel array, +0x310 object; per channel k (Ch = *(L+0x80) + 0x70 k): own = whether
            // *(Ch+0x20)+0x80 == Ch (0x441FB0 crossfades only then), clip *(Ch+0x24) with its keys (clip +0x32 u16) and
            // time per key (+0x34 u16), +0x28 clip time (100 = one 30 Hz frame), +0x2C, +0x50 floats, +0x58 u16 flags.
            if (getenv("ANIM") && p) {
                { DWORD g = rd32(p + 0x50); printf("\tvar_gb90=%lx", g ? rd32(g + 0xB90) : 0); } // *(*(Instance+0x50)+0xB90): bit 0x4000000 read by the anim graph's module-1 native 7
                DWORD comp = rd32(p + 0x280), L = comp ? rd32(comp + 0x14) : 0;
                if (L) {
                    unsigned char b8[4] = {0}; ReadProcessMemory(h, (void*)(L + 0xF6), b8, 3, 0);
                    unsigned char ov = 0, busy = 0; ReadProcessMemory(h, (void*)(L + 0x314), &ov, 1, 0); ReadProcessMemory(h, (void*)(L + 0x300), &busy, 1, 0);
                    DWORD chs = rd32(L + 0x80);
                    printf("\tvar_L=%lx\tvar_Lpat=%ld\tvar_La4=%.4f\tvar_La8=%.4f\tvar_Lac=%.4f\tvar_Lovr=%d\tvar_Lnch=%u\tvar_Lcur=%u"
                           "\tvar_Lbusy=%u\tvar_Lqr=%ld\tvar_Lqw=%ld\tvar_Lch=%lx\tvar_Lobj=%lx",
                           L, (long)rd32(L + 0x44), rdf(L + 0xA4), rdf(L + 0xA8), rdf(L + 0xAC), (int)(signed char)ov, b8[0], b8[2], busy,
                           (long)rd32(L + 0x2F4), (long)rd32(L + 0x2F8), chs, rd32(L + 0x310));
                    for (int k = 0; k < b8[0] && k < 8 && chs; ++k) {
                        DWORD ch = chs + 0x70u * k, own = rd32(ch + 0x20), clip = rd32(ch + 0x24);
                        printf("\tvar_c%down=%d\tvar_c%dclip=%lx\tvar_c%dkeys=%u\tvar_c%dkt=%u\tvar_c%dt=%.3f\tvar_c%d2c=%.4f\tvar_c%d50=%.4f\tvar_c%dfl=%x",
                               k, own ? rd32(own + 0x80) == ch : 0, k, clip, k, clip ? (unsigned short)rd16(clip + 0x32) : 0,
                               k, clip ? (unsigned short)rd16(clip + 0x34) : 0, k, rdf(ch + 0x28), k, rdf(ch + 0x2C), k, rdf(ch + 0x50),
                               k, (unsigned short)rd16(ch + 0x58));
                    }
                }
            }
            if (rayCave) { // RAY=1: the wall-under-hang ray's calls and results since the last tick
                DWORD wp = rd32(rayCave + 0xF00); if (wp - rayRead > 256) rayRead = wp - 256;
                printf("\tvar_ray=");
                for (; rayRead < wp; ++rayRead) {
                    DWORD r[32]; ReadProcessMemory(h, (void*)(rayCave + 0x1000 + 128 * (rayRead & 255)), r, 128, 0);
                    if (r[0] == 1) { printf("E@%lu", r[1]); for (int k = 2; k < 21; ++k) printf(":%lx", r[k]); }
                    else { printf("R@%lu:%lx", r[1], r[2 + 4]); for (int k = 2; k < 26; ++k) printf(":%lx", r[k]); }
                    putchar(';');
                }
            }
            if (callCave) { // CALLS=1: the call-log ring's entries since the last tick
                DWORD wp = rd32(callCave + 0x1F00); if (wp - callRead > 256) callRead = wp - 256;
                printf("\tvar_calls=");
                for (; callRead < wp; ++callRead) {
                    DWORD r[32]; ReadProcessMemory(h, (void*)(callCave + 0x2000 + 128 * (callRead & 255)), r, 128, 0);
                    printf("%lx:%lx:%lx:%lx:%lx:%lx:%lx", r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
                    int fi = -1; for (int f = 0; f < gN; ++f) if (gFn[f] == r[0]) fi = f;
                    if (fi >= 0) for (int d = 0; d < 2; ++d) if (gDeref[fi][d]) printf(":%lx:%lx:%lx:%lx", r[9 + 4 * d], r[10 + 4 * d], r[11 + 4 * d], r[12 + 4 * d]);
                    printf("^%lx", r[8]);
                    if (fi >= 0 && gRes[fi]) { if (r[25]) { printf("="); for (int k = 0; k < 8; ++k) printf(k ? ":%lx" : "%lx", r[17 + k]); } else printf("=?"); }
                    printf("@%lu;", r[7]);
                }
            }
            DWORD L = p && rd32(p + 0x280) ? rd32(rd32(p + 0x280) + 0x14) : 0, sc = rd32(0xFADFB0) ? rd32(rd32(0xFADFB0) + 0xB0) : 0;
            for (int i = 0; i < nv; ++i) {
                char bc = vars[i].base;
                DWORD base = bc == 'd' ? pd : bc == 'p' ? p : bc == 'c' ? c1 : bc == 'L' ? L : bc == 'r' ? (L ? rd32(L + 0x7C) : 0)
                           : bc == 'k' ? (L ? rd32(L + 0x80) : 0) : bc == 's' ? sc : 0;
                DWORD v = bc == 'a' ? rd32(vars[i].off) : base ? rd32(base + vars[i].off) : 0; // a: absolute address
                if (vars[i].isF) { float fv; memcpy(&fv, &v, 4); printf("\tvar_%s=%g", vars[i].name, fv); }
                else printf("\tvar_%s=%lx", vars[i].name, v);
            }
            if (getenv("STICKDUMP")) { // STICKDUMP=len: hex bytes of the stick controller *(*(0xFADFB0)+0xB0) from +0
                int len = atoi(getenv("STICKDUMP")); if (len < 0 || len > 0x400) len = 0x1A0;
                unsigned char b[0x400] = {0}; if (sc) ReadProcessMemory(h, (void*)sc, b, len, 0);
                printf("\tvar_stick=%lx:", sc); for (int i = 0; i < len; ++i) printf("%02x", b[i]);
            }
            if (getenv("CAMDUMP") && atoi(getenv("CAMDUMP"))) { // CAMDUMP=1 (ChartA): the matrices 0x5E7850 / 0x5E7870 return, flag and floats
                // 0x5E7850 / 0x5E7870: if byte 0xE8EEC8 then 0xE8EE30 / 0xE8EE70, else g_camera (*(0xE80534)) vtable +0x38 /
                // +0x3C; those getters are decoded as `lea eax,[ecx+d8|d32]; ret` (else their first bytes are printed).
                unsigned char fl = 0; ReadProcessMemory(h, (void*)0xE8EEC8, &fl, 1, 0);
                DWORD cm = rd32(0xE80534), m[2] = {0xE8EE30, 0xE8EE70};
                if (!fl) for (int k = 0; k < 2; ++k) {
                    DWORD fn = cm ? rd32(rd32(cm) + 0x38 + 4 * k) : 0; unsigned char b[8] = {0}; if (fn) ReadProcessMemory(h, (void*)fn, b, 8, 0);
                    m[k] = b[0] == 0x8D && b[1] == 0x41 && b[3] == 0xC3 ? cm + (signed char)b[2]
                         : b[0] == 0x8D && b[1] == 0x81 && b[6] == 0xC3 ? cm + (b[2] | b[3] << 8 | b[4] << 16 | (DWORD)b[5] << 24) : 0;
                    if (!m[k]) { printf("\tvar_camfn%d=%lx:", k, fn); for (int i = 0; i < 8; ++i) printf("%02x", b[i]); }
                }
                printf("\tvar_camflag=%u\tvar_cam=%lx\tvar_f8ac298=%g\tvar_f8b0444=%g\tvar_f8b0448=%g", fl, cm, rdf(0x8AC298), rdf(0x8B0444), rdf(0x8B0448));
                for (int k = 0; k < 2; ++k) { float mm[16] = {0}; if (m[k]) ReadProcessMemory(h, (void*)m[k], mm, 64, 0);
                    printf("\tvar_cam%s=%lx:", k ? "3c" : "38", m[k]); for (int i = 0; i < 16; ++i) printf(i ? ",%g" : "%g", mm[i]); }
            }
        if (getenv("SCANAT") && t == atoi(getenv("SCANAT"))) { // debug: where else is a position (x, y) stored?
            float sx, sy; sscanf(getenv("SCANV"), "%f,%f", &sx, &sy);
            DWORD bases[3] = {p, p ? rd32(p + 0x288) : 0, pd}; const char* bn[3] = {"p", "q", "pd"};
            for (int k = 0; k < 3; ++k) { static float buf[0x1000]; if (!bases[k]) continue;
                memset(buf, 0, sizeof buf); ReadProcessMemory(h, (void*)bases[k], buf, sizeof buf, 0);
                for (int i = 0; i + 1 < 0x1000; ++i) if (buf[i] > sx - 2 && buf[i] < sx + 2 && buf[i + 1] > sy - 2 && buf[i + 1] < sy + 2)
                    fprintf(stderr, "scan %s+%x %g %g %g\n", bn[k], i * 4, buf[i], buf[i + 1], buf[i + 2]); }
        }
            putchar('\n'); fflush(stdout);
        }
        // keys for the next frame (tick t + 1): a key is down over [down, up)
        int changed = 0;
        for (int i = 0; i < ne; ++i) {
            if (st[i] == 0 && t + 1 >= dn[i]) { sendKey(vk[i], 0); mask |= maskOf(vk[i]); st[i] = 1; changed = 1; }
            if (st[i] == 1 && t + 1 >= up[i]) { sendKey(vk[i], 1); mask &= ~maskOf(vk[i]); st[i] = 2; changed = 1; }
        }
        if (changed && keyWait > 0) Sleep(keyWait);
        w32(AK, f);
    }
    for (int i = 0; i < ne; ++i) if (st[i] == 1) sendKey(vk[i], 1);
    w32(EN, 0); w32(AK, rd32(PO));
    if (fixed) { w32(GT + 0xC8, oldVar); w32(GT + 0x120, oldLock); }
    timeEndPeriod(1);
    (void)f0;
    return lost;
}
