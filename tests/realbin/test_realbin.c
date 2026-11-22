/* Integration test against the user's own, unmodified game binaries.
 *
 * The game files are copyrighted and are never part of this repository, so
 * this test only runs where a game installation is available:
 *
 *     fc3pp_realbin "<game>\bin" ["<other game>\bin" ...]
 *     set FC3PP_GAME_BIN=<dir>[;<dir>...]
 *
 * Without any directory it exits with code 77 ("skipped").
 *
 * For every engine module it
 *   1. maps the real DLL without running any of its code
 *      (DONT_RESOLVE_DLL_REFERENCES),
 *   2. checks that the processor-enumeration loop is found exactly once,
 *   3. calls the game's real CPU-count function with the five Win32 imports
 *      it uses replaced by fakes that simulate a 16- and a 32-processor
 *      machine - before the patch (32 processors: never returns) and after
 *      (returns, 32 processors counted, 16-processor results unchanged).
 *
 * Nothing is written to the game folder.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "patches.h"
#include "pe_file.h"
#include "pe_image.h"
#include "tex_rule.h"
#include "tex_stale.h"
#include "texguard.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                 \
    do {                                 \
        ++g_checks;                      \
        if (!(cond)) {                   \
            ++g_failures;                \
            printf("    FAIL: ");        \
            printf(__VA_ARGS__);         \
            printf("\n");                \
        }                                \
    } while (0)

/* ---- fakes for the imports used by the CPU-count function ------------------ */

static DWORD_PTR g_fake_mask;       /* simulated process == system affinity */
static volatile LONG g_affinity_calls;
static LONG g_affinity_success_limit;

static HANDLE WINAPI fake_GetCurrentProcess(void)
{
    return (HANDLE)(LONG_PTR)-1;
}

static HANDLE WINAPI fake_GetCurrentThread(void)
{
    return (HANDLE)(LONG_PTR)-2;
}

static BOOL WINAPI fake_GetProcessAffinityMask(HANDLE process, PDWORD_PTR process_mask, PDWORD_PTR system_mask)
{
    (void)process;
    *process_mask = g_fake_mask;
    *system_mask = g_fake_mask;
    return TRUE;
}

/* Succeeds for as many calls as the simulated machine has processors, then
 * fails: on a real 64-thread machine every (wrapped) mask would succeed and
 * the unpatched loop would overrun its 256-entry stack tables, which is not
 * something a test should reproduce. Failing keeps the runaway loop harmless
 * while it is still clearly observable as "never returns". */
static volatile LONG g_abort;

static DWORD_PTR WINAPI fake_SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask)
{
    LONG n = InterlockedIncrement(&g_affinity_calls);
    (void)thread;
    (void)mask;
    if (g_abort) {
        ExitThread(2); /* ends a loop that would otherwise spin forever */
    }
    return n <= g_affinity_success_limit ? 1 : 0;
}

static VOID WINAPI fake_Sleep(DWORD ms)
{
    (void)ms;
}

/* ---- running the game's function -------------------------------------------- */

typedef unsigned char(__cdecl *cpu_count_fn)(unsigned *logical, unsigned *cores, unsigned *packages);

typedef struct call_ctx {
    cpu_count_fn fn;
    unsigned logical, cores, packages;
    unsigned char ok;
} call_ctx;

/* The four builds of this function do not agree on a calling convention:
 * three take their arguments on the stack (and differ in who cleans it up),
 * one was given a register convention by link-time optimisation (ecx, edx,
 * then one stack argument). This position-independent trampoline handles all
 * of them and restores the stack pointer itself:
 *
 *     unsigned __cdecl call(void *fn, void *a1, void *a2, void *a3, int register_args);
 */
static const unsigned char TRAMPOLINE[] = {
    0x55,                   /* 00 push ebp                         */
    0x8B, 0xEC,             /* 01 mov  ebp, esp                    */
    0x83, 0x7D, 0x18, 0x00, /* 03 cmp  dword [ebp+18h], 0          */
    0x75, 0x0B,             /* 07 jne  register_args               */
    0xFF, 0x75, 0x14,       /* 09 push dword [ebp+14h]   ; a3      */
    0xFF, 0x75, 0x10,       /* 0C push dword [ebp+10h]   ; a2      */
    0xFF, 0x75, 0x0C,       /* 0F push dword [ebp+0Ch]   ; a1      */
    0xEB, 0x09,             /* 12 jmp  call                        */
    0xFF, 0x75, 0x14,       /* 14 push dword [ebp+14h]   ; a3      */
    0x8B, 0x55, 0x10,       /* 17 mov  edx, [ebp+10h]    ; a2      */
    0x8B, 0x4D, 0x0C,       /* 1A mov  ecx, [ebp+0Ch]    ; a1      */
    0xFF, 0x55, 0x08,       /* 1D call dword [ebp+8]               */
    0x8B, 0xE5,             /* 20 mov  esp, ebp   ; whoever was meant to pop the arguments */
    0x5D,                   /* 22 pop  ebp                         */
    0xC3,                   /* 23 ret                              */
};

typedef unsigned(__cdecl *trampoline_fn)(void *fn, void *a1, void *a2, void *a3, int register_args);

static trampoline_fn g_trampoline;
static int g_register_args;

static DWORD WINAPI call_thread(LPVOID arg)
{
    call_ctx *c = (call_ctx *)arg;
    c->ok = (unsigned char)g_trampoline((void *)c->fn, &c->logical, &c->cores, &c->packages, g_register_args);
    return 0;
}

/* Returns 1 if the function returned, 0 if it was still looping after far more
 * iterations than any real machine could need (and had to be stopped). */
static int call_cpu_count(cpu_count_fn fn, unsigned long mask, unsigned processors, call_ctx *out)
{
    HANDLE thread;
    DWORD wait;
    int spins;

    memset(out, 0, sizeof(*out));
    out->fn = fn;
    g_fake_mask = mask;
    g_affinity_calls = 0;
    g_affinity_success_limit = (LONG)processors;
    /* The debug build of the function needs well over 3 KB of stack per call
     * plus run-time checks; give the thread plenty. */
    thread = CreateThread(NULL, 1 << 20, call_thread, out, 0, NULL);
    if (!thread) {
        return -1;
    }
    wait = WaitForSingleObject(thread, 100);
    for (spins = 0; wait == WAIT_TIMEOUT && spins < 100 && g_affinity_calls < 2000000; ++spins) {
        wait = WaitForSingleObject(thread, 100);
    }
    if (wait == WAIT_TIMEOUT) {
        /* Ask the thread to leave from inside the game function's next call
         * into our fake; nothing is killed from outside. */
        InterlockedExchange(&g_abort, 1);
        wait = WaitForSingleObject(thread, 10000);
        InterlockedExchange(&g_abort, 0);
        CloseHandle(thread);
        return wait == WAIT_TIMEOUT ? -1 : 0;
    }
    CloseHandle(thread);
    return 1;
}

/* The order of the three output arguments differs between builds, so look for
 * the processor count in any of them. */
static int reports(const call_ctx *c, unsigned processors)
{
    return c->logical == processors || c->cores == processors || c->packages == processors;
}

/* Walks back from the loop to "push ebp / mov ebp,esp / sub esp,imm" that
 * directly follows the end of the previous function. */
static unsigned char *function_start(unsigned char *text, const unsigned char *site)
{
    const unsigned char *p;
    for (p = site; p > text + 3 && site - p < 0x1000; --p) {
        int prologue = p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && (p[3] == 0x81 || p[3] == 0x83) && p[4] == 0xEC;
        int after_end = p[-1] == 0xCC || p[-1] == 0xC3 || p[-1] == 0x90 || p[-3] == 0xC2;
        if (prologue && after_end) {
            return (unsigned char *)p;
        }
    }
    return NULL;
}

/* True when the function reads ecx and edx before writing them, i.e. it was
 * compiled to take its first two arguments in registers. */
static int takes_register_args(const unsigned char *start)
{
    int from_ecx = 0, from_edx = 0;
    int i;
    /* Only the prologue counts: the argument registers are dead after the
     * first call, which comes well after this window in every known build. */
    for (i = 3; i < 0x30; ++i) {
        if (start[i] == 0x8B && (start[i + 1] == 0xF9 || start[i + 1] == 0xD9 || start[i + 1] == 0xF1)) {
            from_ecx = 1; /* mov edi|ebx|esi, ecx */
        }
        if (start[i] == 0x8B && (start[i + 1] == 0xDA || start[i + 1] == 0xFA || start[i + 1] == 0xF2)) {
            from_edx = 1; /* mov ebx|edi|esi, edx */
        }
    }
    return from_ecx && from_edx;
}

/* The module is mapped without resolving its imports, so anything the function
 * calls that we did not provide faults. Report where instead of just dying. */
static HMODULE g_module_under_test;

static LONG WINAPI crash_reporter(EXCEPTION_POINTERS *info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW) {
        ULONG_PTR ip = (ULONG_PTR)info->ExceptionRecord->ExceptionAddress;
        ULONG_PTR ret = 0;
        if (!IsBadReadPtr((const void *)info->ContextRecord->Esp, sizeof(ret))) {
            ret = *(const ULONG_PTR *)info->ContextRecord->Esp;
        }
        printf("    FAIL: exception 0x%08lX at 0x%08lX (module base 0x%08lX, return address RVA 0x%08lX)\n",
               (unsigned long)code, (unsigned long)ip, (unsigned long)(ULONG_PTR)g_module_under_test,
               (unsigned long)(ret - (ULONG_PTR)g_module_under_test));
        printf("    the game function called something this test does not provide\n");
        ExitProcess(1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int hook_fakes(HMODULE mod)
{
    static const char k32[] = "KERNEL32.dll";
    int n = 0;
    /* The optimised build calls the C runtime's memset through its import
     * table; give it ours. (The debug build links the runtime statically.) */
    pei_hook_import(mod, "MSVCR100.dll", "memset", (void *)memset, NULL);
    pei_hook_import(mod, "MSVCR110.dll", "memset", (void *)memset, NULL);
    pei_hook_import(mod, "MSVCR90.dll", "memset", (void *)memset, NULL);
    n += pei_hook_import(mod, k32, "GetCurrentProcess", (void *)fake_GetCurrentProcess, NULL) == 1;
    n += pei_hook_import(mod, k32, "GetCurrentThread", (void *)fake_GetCurrentThread, NULL) == 1;
    n += pei_hook_import(mod, k32, "GetProcessAffinityMask", (void *)fake_GetProcessAffinityMask, NULL) == 1;
    n += pei_hook_import(mod, k32, "SetThreadAffinityMask", (void *)fake_SetThreadAffinityMask, NULL) == 1;
    n += pei_hook_import(mod, k32, "Sleep", (void *)fake_Sleep, NULL) == 1;
    return n;
}

/* ---- the texture guard on the game's real texture class ----------------------------
 * The guard is installed in the mapped module, then the game's own virtual
 * table is used to hand a made-up texture object a top mip level that does not
 * fit. The real method behind the slot would go on to use engine globals that
 * do not exist in this process (and crash, which crash_reporter turns into a
 * failure), so "returned 0 and nothing happened" proves the guard sits in
 * front of it with the right calling convention. */

typedef struct fake_d3d_texture {
    void *const *vtbl;
    UINT width, height, levels, format;
    LONG desc_calls;
} fake_d3d_texture;

typedef struct fake_desc { /* D3D11_TEXTURE2D_DESC */
    UINT width, height, levels, array_size, format, sample_count, sample_quality, usage, bind, cpu, misc;
} fake_desc;

static void __stdcall fake_get_desc(fake_d3d_texture *self, fake_desc *desc)
{
    memset(desc, 0, sizeof(*desc));
    desc->width = self->width;
    desc->height = self->height;
    desc->levels = self->levels;
    desc->array_size = 1;
    desc->format = self->format;
    desc->sample_count = 1;
    InterlockedIncrement(&self->desc_calls);
}

static void *const FAKE_D3D_VTBL[11] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, (void *)fake_get_desc,
};

typedef unsigned char(__fastcall *tex_add_fn)(void *self, void *unused, const void *dds, unsigned size, void *buffer,
                                              unsigned flag);
typedef unsigned char(__fastcall *tex_drop_fn)(void *self, void *unused);

static void test_texture_guard(HMODULE mod, void **table, unsigned add_slot)
{
    static unsigned char dds[128];
    fake_d3d_texture d3d;
    void *object[16]; /* the engine's texture object: table at +0, Direct3D texture at +1Ch */
    void *add_before = table[add_slot], *drop_before = table[add_slot + 1], *destroy_before = table[0];
    unsigned width = 1024, height = 1024;
    volatile unsigned canary = 0xC0FFEE11u;
    unsigned char r;

    CHECK(texguard_install(mod, "engine", 0) == 1, "the texture guard did not install on the real module");
    CHECK(table[add_slot] != add_before && table[add_slot + 1] != drop_before && table[0] != destroy_before,
          "the texture class table was not changed");
    if (table[add_slot] == add_before) {
        return;
    }
    CHECK(texguard_install(mod, "engine", 0) == 1, "installing twice is not harmless");

    memset(&d3d, 0, sizeof(d3d));
    d3d.vtbl = FAKE_D3D_VTBL;
    d3d.width = 2048; /* what a texture mod puts in place of a 512 texture */
    d3d.height = 2048;
    d3d.levels = 11;
    d3d.format = 70; /* BC1 */
    memset(object, 0, sizeof(object));
    object[0] = table;
    object[TEX_OFFSET_D3D_TEXTURE / sizeof(void *)] = &d3d;

    memset(dds, 0, sizeof(dds)); /* the game's original 1024x1024 DXT1 top level */
    memcpy(dds, "DDS ", 4);
    dds[4] = 124;
    memcpy(dds + 12, &height, 4);
    memcpy(dds + 16, &width, 4);
    dds[28] = 1;
    dds[76] = 32;
    dds[80] = 4;
    memcpy(dds + 84, "DXT1", 4);

    r = ((tex_add_fn)table[add_slot])(object, NULL, dds, sizeof(dds), NULL, 0);
    CHECK(r == 0, "a top level that is too small was not refused");
    CHECK(d3d.desc_calls == 1, "the texture was asked for its description %ld times, expected once",
          (long)d3d.desc_calls);
    r = ((tex_drop_fn)table[add_slot + 1])(object, NULL);
    CHECK(r == 0, "the unload that follows a refused level was not ignored");
    CHECK(d3d.desc_calls == 1, "the unload that follows a refused level reached the game's method");
    CHECK(canary == 0xC0FFEE11u, "the stack was disturbed by the calls");
    printf("    texture guard: installed on the game's table; a 1024x1024 top level for a 2048x2048 texture is "
           "refused before the game's code runs, and the unload that follows is ignored\n");
}

static void test_engine_module(const wchar_t *dir, const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    HMODULE mod;
    pei_section sec[32];
    unsigned nsec, i;
    unsigned sites = 0;
    unsigned char *text = NULL;
    size_t text_size = 0;
    patch_report rep;
    const char *variant = NULL;

    _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
    path[MAX_PATH - 1] = L'\0';
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    printf("  %ls\n", name);
    mod = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(mod != NULL, "could not map the module (error %lu)", (unsigned long)GetLastError());
    if (!mod) {
        return;
    }
    CHECK(pei_valid(mod), "not a 32-bit PE image");
    g_module_under_test = mod;

    nsec = pei_sections(mod, sec, 32);
    for (i = 0; i < nsec && i < 32; ++i) {
        if (!(sec[i].characteristics & IMAGE_SCN_MEM_EXECUTE) || !sec[i].size) {
            continue;
        }
        patch_enum_loop(sec[i].start, sec[i].size, NULL, NULL, &rep); /* dry run */
        if (rep.applied) {
            sites += rep.applied;
            text = sec[i].start;
            text_size = sec[i].size;
            variant = rep.variant;
        }
        CHECK(rep.already == 0 && rep.foreign == 0, "the module on disk is already modified");
        CHECK(rep.rejected == 0, "%u signature candidate(s) failed validation", rep.rejected);
    }
    CHECK(sites == 1, "expected exactly one enumeration loop, found %u", sites);
    if (sites != 1 || !text) {
        FreeLibrary(mod);
        return;
    }
    patch_enum_loop(text, text_size, NULL, NULL, &rep);
    printf("    enumeration loop: '%s' variant at RVA 0x%08lX\n", variant,
           (unsigned long)(rep.site - (const unsigned char *)mod));

    {
        unsigned char *start = function_start(text, rep.site);
        cpu_count_fn fn = (cpu_count_fn)(void *)start;
        call_ctx before16, hang32, after16, after32, after31;
        int r;
        int failures_before = g_failures;

        CHECK(start != NULL, "could not locate the start of the CPU-count function");
        CHECK(hook_fakes(mod) == 5, "the function's five imports were not all found");
        if (!start || g_failures != failures_before) {
            FreeLibrary(mod);
            return;
        }
        g_register_args = takes_register_args(start);
        if (g_register_args) {
            printf("    (this build passes two arguments in registers)\n");
        }

        /* --- before the patch --- */
        r = call_cpu_count(fn, 0xFFFFul, 16, &before16);
        CHECK(r == 1 && before16.ok == 1, "unpatched function failed on a 16-processor machine");
        CHECK(reports(&before16, 16), "unpatched: did not count 16 logical processors (%u/%u/%u)", before16.logical,
              before16.cores, before16.packages);
        r = call_cpu_count(fn, 0xFFFFFFFFul, 32, &hang32);
        CHECK(r == 0, "unpatched function returned on a 32-processor machine; the bug was not reproduced");
        printf("    unpatched, 16 processors: returns (%u logical, %u cores, %u packages)\n", before16.logical,
               before16.cores, before16.packages);
        printf("    unpatched, 32 processors: %s\n", r == 0 ? "never returns (bug reproduced)" : "returned");

        /* --- patch (in this process's private copy of the mapped image) --- */
        CHECK(patch_enum_loop(text, text_size, mem_patch_writer, NULL, &rep) == 1 && rep.applied == 1,
              "patch was not applied");

        /* --- after the patch --- */
        r = call_cpu_count(fn, 0xFFFFul, 16, &after16);
        CHECK(r == 1 && after16.ok == 1, "patched function failed on a 16-processor machine");
        CHECK(after16.logical == before16.logical && after16.cores == before16.cores &&
                  after16.packages == before16.packages,
              "patched results differ on a 16-processor machine (%u/%u/%u vs %u/%u/%u)", after16.logical,
              after16.cores, after16.packages, before16.logical, before16.cores, before16.packages);
        r = call_cpu_count(fn, 0x7FFFFFFFul, 31, &after31);
        CHECK(r == 1 && after31.ok == 1 && reports(&after31, 31), "patched: 31 processors gave %u/%u/%u",
              after31.logical, after31.cores, after31.packages);
        r = call_cpu_count(fn, 0xFFFFFFFFul, 32, &after32);
        CHECK(r == 1, "patched function still does not return on a 32-processor machine");
        CHECK(r == 1 && after32.ok == 1 && reports(&after32, 32),
              "patched: did not count 32 logical processors (%u/%u/%u)", after32.logical, after32.cores,
              after32.packages);
        /* one probe per processor, plus the call that restores the original affinity */
        CHECK(g_affinity_calls == 33, "patched: %ld affinity calls, expected exactly 33", (long)g_affinity_calls);
        printf("    patched,   16 processors: returns (%u logical, %u cores, %u packages) - unchanged\n",
               after16.logical, after16.cores, after16.packages);
        printf("    patched,   32 processors: %s (%u logical, %u cores, %u packages)\n",
               r == 1 ? "returns" : "STILL HANGS", after32.logical, after32.cores, after32.packages);

        /* idempotent */
        CHECK(patch_enum_loop(text, text_size, mem_patch_writer, NULL, &rep) == 1 && rep.already == 1 &&
                  rep.applied == 0,
              "second patch pass was not a no-op");
    }
    if (_wcsicmp(name, L"FC3_d3d11.dll") == 0) {
        /* The fullscreen guard reaches the renderer's swap chain through this import. */
        int present = pei_find_import(mod, "dxgi.dll", "CreateDXGIFactory1") != NULL;
        CHECK(present, "the DX11 engine module does not import dxgi.dll!CreateDXGIFactory1");
        printf("    DXGI factory import: %s\n", present ? "found (fullscreen guard can attach)" : "MISSING");
    }
    if (_wcsicmp(name, L"FC3_d3d11.dll") == 0 || _wcsicmp(name, L"FC3.dll") == 0) {
        /* The in-game menu keeps the game from seeing its input through these two. */
        int pad = pei_find_import(mod, "xinput1_3.dll", "#2") != NULL; /* XInputGetState, by ordinal */
        int keys = pei_find_import(mod, "dinput8.dll", "DirectInput8Create") != NULL;
        CHECK(pad, "the engine module does not import XInputGetState (ordinal 2) from xinput1_3.dll");
        CHECK(keys, "the engine module does not import dinput8.dll!DirectInput8Create");
        CHECK(pei_find_import(mod, "xinput1_3.dll", "#99") == NULL, "an ordinal that is not imported was found");
        printf("    input imports: gamepad %s, keyboard and mouse %s\n", pad ? "found" : "MISSING",
               keys ? "found" : "MISSING");
    }
    {
        /* The texture guard wraps two methods of the DX11 renderer's texture
         * class, found by signature and through the class's virtual table. */
        tex_class_site site;
        unsigned long slot = 0;
        unsigned slots = 0;
        int found = 0;
        int dx11 = _wcsicmp(name, L"FC3_d3d11.dll") == 0;

        memset(&site, 0, sizeof(site));
        for (i = 0; i < nsec && i < 32 && found == 0; ++i) {
            if ((sec[i].characteristics & IMAGE_SCN_MEM_EXECUTE) && sec[i].size) {
                found = tex_find_methods(sec[i].start, sec[i].size, (unsigned long)(ULONG_PTR)sec[i].start, &site);
            }
        }
        for (i = 0; i < nsec && i < 32 && found == 1; ++i) {
            if (sec[i].size && (sec[i].characteristics & IMAGE_SCN_MEM_READ) &&
                !(sec[i].characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE))) {
                unsigned long first = 0;
                unsigned n = tex_find_slots(sec[i].start, sec[i].size, (unsigned long)(ULONG_PTR)sec[i].start, &site,
                                            &first);
                if (n && !slots) {
                    slot = first;
                }
                slots += n;
            }
        }
        if (dx11) {
            CHECK(found == 1, "texture class methods: %s",
                  found == 0 ? "not found" : "more than one candidate");
            CHECK(found != 1 || slots == 1, "texture class: %u virtual tables hold the two methods, expected 1", slots);
            if (found == 1 && slots == 1) {
                void **table = (void **)(ULONG_PTR)slot - site.add_slot;
                const unsigned char *destroy = (const unsigned char *)table[TEX_SLOT_DESTROY];
                const unsigned char *base = (const unsigned char *)mod;
                int in_code = 0;
                for (i = 0; i < nsec && i < 32; ++i) {
                    if ((sec[i].characteristics & IMAGE_SCN_MEM_EXECUTE) && destroy >= sec[i].start &&
                        destroy + 32 <= sec[i].start + sec[i].size) {
                        in_code = 1;
                    }
                }
                CHECK(table[site.add_slot] == (void *)(ULONG_PTR)site.add &&
                          table[site.add_slot + 1] == (void *)(ULONG_PTR)site.drop,
                      "texture class: the two methods are not in the expected slots");
                CHECK(in_code && tex_is_destructor(destroy, 32),
                      "texture class: the table does not start with a destructor %u slots before the add method",
                      site.add_slot);
                printf("    texture class: '%s' variant, add at RVA 0x%08lX (slot %u), drop at RVA 0x%08lX, "
                       "table at RVA 0x%08lX, destructor %s\n",
                       site.variant, site.add - (unsigned long)(ULONG_PTR)mod, site.add_slot,
                       site.drop - (unsigned long)(ULONG_PTR)mod,
                       (unsigned long)((const unsigned char *)table - base),
                       in_code && tex_is_destructor(destroy, 32) ? "confirmed" : "NOT RECOGNISED");
            }
        } else if (_wcsicmp(name, L"FC3.dll") == 0) {
            CHECK(found == 0, "the DX9 engine module unexpectedly matches the DX11 texture class signatures");
            printf("    texture class: not present (DX9 renderer; the texture guard is DX11 only)\n");
        }
        if (dx11 && found == 1 && slots == 1) {
            test_texture_guard(mod, (void **)(ULONG_PTR)slot - site.add_slot, site.add_slot);
        }
    }
    FreeLibrary(mod);
}

static unsigned char *read_file(const wchar_t *path, DWORD *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    unsigned char *buf = NULL;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    *size = GetFileSize(h, NULL);
    if (*size != INVALID_FILE_SIZE && *size > 0 && *size < (64u << 20)) {
        buf = (unsigned char *)malloc(*size);
        if (buf && (!ReadFile(h, buf, *size, &got, NULL) || got != *size)) {
            free(buf);
            buf = NULL;
        }
    }
    CloseHandle(h);
    return buf;
}

/* The executables: header parsing, checksum algorithm and the LAA patch on an
 * in-memory copy. */
static void test_executable(const wchar_t *dir, const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    unsigned char *buf, *copy;
    DWORD size = 0;
    unsigned long stored = 0, computed = 0;
    int laa = -1, changed = 0;
    DWORD i, diffs = 0;

    _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
    path[MAX_PATH - 1] = L'\0';
    buf = read_file(path, &size);
    if (!buf) {
        return;
    }
    printf("  %ls\n", name);
    CHECK(pef_get_laa(buf, size, &laa) == PEF_OK, "not recognised as a 32-bit executable");
    CHECK(pef_stored_checksum(buf, size, &stored) == PEF_OK, "no checksum field");
    CHECK(pef_compute_checksum(buf, size, &computed) == PEF_OK, "checksum computation failed");
    if (!laa) {
        /* An untouched file: our checksum must reproduce the linker's. */
        CHECK(stored == computed, "checksum mismatch: stored 0x%08lX, computed 0x%08lX", stored, computed);
    }
    printf("    large address aware: %s, checksum 0x%08lX %s\n", laa ? "yes" : "no", stored,
           stored == computed ? "(verified)" : "(differs)");

    copy = (unsigned char *)malloc(size);
    if (copy) {
        unsigned long stored2 = 0, computed2 = 0;
        int laa2 = 0;
        memcpy(copy, buf, size);
        CHECK(pef_set_laa(copy, size, 1, &changed) == PEF_OK, "LAA patch failed");
        CHECK(pef_get_laa(copy, size, &laa2) == PEF_OK && laa2 == 1, "LAA flag not set after patch");
        for (i = 0; i < size; ++i) {
            diffs += buf[i] != copy[i];
        }
        if (!laa) {
            /* one flag byte plus at most the four checksum bytes */
            CHECK(changed == 1 && diffs >= 1 && diffs <= 5, "LAA patch changed %lu bytes", (unsigned long)diffs);
            pef_stored_checksum(copy, size, &stored2);
            pef_compute_checksum(copy, size, &computed2);
            CHECK(stored2 == computed2, "checksum not valid after the LAA patch");
            printf("    LAA patch on a copy: %lu byte(s) changed, checksum 0x%08lX valid\n", (unsigned long)diffs,
                   stored2);
        } else {
            CHECK(changed == 0 && diffs == 0, "patching an already patched file changed it");
        }
        free(copy);
    }
    free(buf);
}

/* The scan for top mip level files that belong to textures a mod has replaced,
 * on the game's real archives. What it finds depends on the mods installed,
 * so this checks that the archives are understood and the numbers add up. */
static void test_archives(const wchar_t *bin_dir)
{
    static unsigned long long ids[4096];
    wchar_t data_dir[MAX_PATH];
    ts_stats stats;
    unsigned archives = 0;
    DWORD started = GetTickCount();
    long count;

    _snwprintf(data_dir, MAX_PATH, L"%ls\\..\\data_win32", bin_dir);
    data_dir[MAX_PATH - 1] = L'\0';
    if (GetFileAttributesW(data_dir) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    count = tex_stale_scan_folder(data_dir, ids, 4096, &stats, &archives);
    printf("  data archives\n");
    CHECK(count >= 0, "the archive scan ran out of memory");
    CHECK(archives >= 2, "only %u archives were read", archives);
    CHECK(stats.linked == stats.own_top + stats.missing + stats.unfit + stats.unchanged + stats.stale,
          "the categories do not add up");
    CHECK(stats.truncated == 0, "%u identities did not fit", stats.truncated);
    CHECK(count <= (long)stats.stale * 8, "more identities than stale pairs can produce");
    printf("    %u archives read in %lu ms; patch archive: %u textures, %u with a separate top mip level\n", archives,
           (unsigned long)(GetTickCount() - started), stats.textures, stats.linked);
    printf("    of those: %u bring their own top level, %u name one that exists nowhere, %u would get an original\n"
           "    of the wrong size, %u are the game's own files, %u replace a texture whose original top level\n"
           "    still fits (%ld files to leave out)\n",
           stats.own_top, stats.missing, stats.unfit, stats.unchanged, stats.stale, count);
}

static void test_directory(const wchar_t *dir)
{
    static const wchar_t *const engine[] = {L"FC3_d3d11.dll", L"FC3.dll", L"systemdetection.dll"};
    static const wchar_t *const exes[] = {L"farcry3.exe", L"farcry3_d3d11.exe", L"fc3_blooddragon.exe",
                                          L"fc3_blooddragon_d3d11.exe"};
    size_t i;
    DWORD attr = GetFileAttributesW(dir);

    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        printf("game folder #%d: not found\n", g_checks);
        ++g_failures;
        return;
    }
    for (i = 0; i < sizeof(engine) / sizeof(engine[0]); ++i) {
        test_engine_module(dir, engine[i]);
    }
    for (i = 0; i < sizeof(exes) / sizeof(exes[0]); ++i) {
        test_executable(dir, exes[i]);
    }
    test_archives(dir);
}

int wmain(int argc, wchar_t **argv)
{
    int dirs = 0;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, crash_reporter);
    {
        void *code = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!code) {
            printf("could not allocate executable memory\n");
            return 1;
        }
        memcpy(code, TRAMPOLINE, sizeof(TRAMPOLINE));
        g_trampoline = (trampoline_fn)code;
    }
    for (i = 1; i < argc; ++i) {
        printf("game folder #%d\n", ++dirs); /* the path itself is deliberately not printed */
        test_directory(argv[i]);
    }
    if (argc < 2) {
        static wchar_t env[4096];
        DWORD n = GetEnvironmentVariableW(L"FC3PP_GAME_BIN", env, 4096);
        if (n > 0 && n < 4096) {
            wchar_t *p = env;
            while (p && *p) {
                wchar_t *semi = wcschr(p, L';');
                if (semi) {
                    *semi = L'\0';
                }
                if (*p) {
                    printf("game folder #%d\n", ++dirs);
                    test_directory(p);
                }
                p = semi ? semi + 1 : NULL;
            }
        }
    }
    if (!dirs) {
        printf("SKIPPED: no game folder given (pass the game's bin folder or set FC3PP_GAME_BIN)\n");
        return 77;
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
