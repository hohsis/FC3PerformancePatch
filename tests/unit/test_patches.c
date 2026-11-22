/* Processor-enumeration loop patch: executed against replicas of the engine
 * loop, before and after patching.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#include "loop_blobs.h"
#include "patches.h"
#include "pe_image.h"
#include "test.h"

typedef unsigned(__stdcall *probe_fn)(unsigned mask);
typedef unsigned(__cdecl *loop_fn)(unsigned system_mask, probe_fn probe);

static const unsigned char BLOB_RELEASE[] = {LOOP_BLOB_RELEASE};
static const unsigned char BLOB_DEBUG[] = {LOOP_BLOB_DEBUG};
static const unsigned char BLOB_DISP32[] = {LOOP_BLOB_DISP32};
static const unsigned char BLOB_EBX[] = {LOOP_BLOB_EBX};

static volatile LONG g_calls;
static volatile LONG g_abort;
static unsigned g_masks[64];

static unsigned __stdcall probe(unsigned mask)
{
    LONG n = InterlockedIncrement(&g_calls);
    if (n >= 1 && n <= 64) {
        g_masks[n - 1] = mask;
    }
    if (g_abort) {
        ExitThread(2); /* ends a loop that would otherwise spin forever */
    }
    return 1;
}

/* Copies a blob into executable memory with padding on both sides. */
static unsigned char *load_blob(const unsigned char *blob, size_t size, size_t *region)
{
    unsigned char *mem = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) {
        return NULL;
    }
    memset(mem, 0xCC, 0x1000);
    memcpy(mem + 0x100, blob, size);
    *region = 0x1000;
    return mem;
}

typedef struct run_ctx {
    loop_fn fn;
    unsigned mask;
    unsigned result;
} run_ctx;

static DWORD WINAPI run_thread(LPVOID arg)
{
    run_ctx *ctx = (run_ctx *)arg;
    ctx->result = ctx->fn(ctx->mask, probe);
    return 0;
}

/* Runs the loop on a worker thread. Returns 1 if it returned, 0 if it was
 * spinning without end and had to be told to leave. */
static int run_loop(unsigned char *mem, unsigned mask, unsigned *result)
{
    static run_ctx ctx; /* static: must outlive a thread that is told to exit */
    HANDLE thread;
    DWORD wait;
    int spins;

    ctx.fn = (loop_fn)(void *)(mem + 0x100);
    ctx.mask = mask;
    ctx.result = 0xDEADBEEF;
    g_calls = 0;
    memset(g_masks, 0, sizeof(g_masks));
    thread = CreateThread(NULL, 0, run_thread, &ctx, 0, NULL);
    if (!thread) {
        return -1;
    }
    wait = WaitForSingleObject(thread, 50);
    for (spins = 0; wait == WAIT_TIMEOUT && spins < 100 && g_calls < 200000; ++spins) {
        wait = WaitForSingleObject(thread, 50);
    }
    if (wait == WAIT_TIMEOUT) {
        InterlockedExchange(&g_abort, 1);
        wait = WaitForSingleObject(thread, 10000);
        InterlockedExchange(&g_abort, 0);
        CloseHandle(thread);
        return wait == WAIT_TIMEOUT ? -1 : 0;
    }
    CloseHandle(thread);
    *result = ctx.result;
    return 1;
}

static void check_fixed_behaviour(unsigned char *mem)
{
    unsigned result = 0;
    unsigned i;

    /* 8 processors */
    CHECK(run_loop(mem, 0xFF, &result) == 1);
    CHECK_EQ_U(result, 8);
    CHECK_EQ_U(g_calls, 8);
    /* 31 processors: the last mask tried is bit 30, then 1<<31 > mask ends it */
    CHECK(run_loop(mem, 0x7FFFFFFF, &result) == 1);
    CHECK_EQ_U(result, 31);
    /* 32 or more processors: the case that used to hang */
    CHECK(run_loop(mem, 0xFFFFFFFF, &result) == 1);
    CHECK_EQ_U(result, 32);
    CHECK_EQ_U(g_calls, 32);
    for (i = 0; i < 32; ++i) {
        CHECK_EQ_U(g_masks[i], 1ul << i); /* every processor visited exactly once, in order */
    }
    /* sparse affinity mask (processors 0..3 of 0x0F only, others unavailable) */
    CHECK(run_loop(mem, 0x0F, &result) == 1);
    CHECK_EQ_U(result, 4);
    /* degenerate */
    CHECK(run_loop(mem, 0, &result) == 1);
    CHECK_EQ_U(result, 0);
}

static void test_variant(const unsigned char *blob, size_t size, size_t site_off, const char *name)
{
    size_t region = 0;
    unsigned char *mem = load_blob(blob, size, &region);
    unsigned char before[0x1000];
    patch_report rep;
    unsigned result = 0;

    CHECK(mem != NULL);
    if (!mem) {
        return;
    }

    /* The replica behaves like the engine before the fix. */
    CHECK(run_loop(mem, 0xFF, &result) == 1);
    CHECK_EQ_U(result, 8);
    CHECK(run_loop(mem, 0x7FFFFFFF, &result) == 1);
    CHECK_EQ_U(result, 31);
    CHECK(run_loop(mem, 0xFFFFFFFF, &result) == 0); /* never returns: the bug */

    /* Dry run: reports the site, changes nothing. */
    memcpy(before, mem, region);
    CHECK_EQ_U(patch_enum_loop(mem, region, NULL, NULL, &rep), 1);
    CHECK_EQ_U(rep.applied, 1);
    CHECK(rep.site == mem + 0x100 + site_off);
    CHECK_STR(rep.variant, name);
    CHECK(memcmp(before, mem, region) == 0);

    /* Apply. */
    CHECK_EQ_U(patch_enum_loop(mem, region, mem_patch_writer, NULL, &rep), 1);
    CHECK_EQ_U(rep.applied, 1);
    CHECK_EQ_U(rep.already, 0);
    CHECK_EQ_U(rep.rejected, 0);
    /* Only the loop tail changed. */
    CHECK(memcmp(before, mem, 0x100 + site_off) == 0);
    CHECK(memcmp(before + 0x100 + size, mem + 0x100 + size, region - 0x100 - size) == 0);
    check_fixed_behaviour(mem);

    /* Idempotent: a second pass recognises its own work. */
    memcpy(before, mem, region);
    CHECK_EQ_U(patch_enum_loop(mem, region, mem_patch_writer, NULL, &rep), 1);
    CHECK_EQ_U(rep.applied, 0);
    CHECK_EQ_U(rep.already, 1);
    CHECK(memcmp(before, mem, region) == 0);

    VirtualFree(mem, 0, MEM_RELEASE);
}

static void test_release_variant(void)
{
    test_variant(BLOB_RELEASE, sizeof(BLOB_RELEASE), LOOP_BLOB_RELEASE_SITE, "release");
}

static void test_debug_variant(void)
{
    test_variant(BLOB_DEBUG, sizeof(BLOB_DEBUG), LOOP_BLOB_DEBUG_SITE, "debug");
}

static void test_disp32_variant(void)
{
    test_variant(BLOB_DISP32, sizeof(BLOB_DISP32), LOOP_BLOB_DISP32_SITE, "release-disp32");
}

static void test_ebx_variant(void)
{
    test_variant(BLOB_EBX, sizeof(BLOB_EBX), LOOP_BLOB_EBX_SITE, "release-ebx");
}

static void test_blob_sizes(void)
{
    CHECK_EQ_U(sizeof(BLOB_RELEASE), LOOP_BLOB_RELEASE_SIZE);
    CHECK_EQ_U(sizeof(BLOB_DEBUG), LOOP_BLOB_DEBUG_SIZE);
    CHECK_EQ_U(sizeof(BLOB_DISP32), LOOP_BLOB_DISP32_SIZE);
    CHECK_EQ_U(sizeof(BLOB_EBX), LOOP_BLOB_EBX_SIZE);
}

/* A known third-party fix turns the release-form jnz into "jmp +4", which
 * makes the loop run once (the game then sees a single processor). */
static void test_upgrades_third_party_release_fix(void)
{
    size_t region = 0;
    unsigned char *mem = load_blob(BLOB_RELEASE, sizeof(BLOB_RELEASE), &region);
    patch_report rep;
    unsigned result = 0;

    CHECK(mem != NULL);
    if (!mem) {
        return;
    }
    mem[0x100 + 0x32] = 0xEB;
    mem[0x100 + 0x33] = 0x04;
    CHECK(run_loop(mem, 0xFF, &result) == 1);
    CHECK_EQ_U(result, 1); /* that fix's side effect: only one processor counted */

    CHECK_EQ_U(patch_enum_loop(mem, region, mem_patch_writer, NULL, &rep), 1);
    CHECK_EQ_U(rep.applied, 1);
    check_fixed_behaviour(mem);
    VirtualFree(mem, 0, MEM_RELEASE);
}

static void test_leaves_third_party_debug_fix(void)
{
    size_t region = 0;
    unsigned char *mem = load_blob(BLOB_DEBUG, sizeof(BLOB_DEBUG), &region);
    unsigned char before[0x1000];
    patch_report rep;

    CHECK(mem != NULL);
    if (!mem) {
        return;
    }
    memset(mem + 0x100 + 0x48, 0, 4); /* jmp +0 */
    memcpy(before, mem, region);
    CHECK_EQ_U(patch_enum_loop(mem, region, mem_patch_writer, NULL, &rep), 0);
    CHECK_EQ_U(rep.foreign, 1);
    CHECK_EQ_U(rep.applied, 0);
    CHECK(memcmp(before, mem, region) == 0);
    VirtualFree(mem, 0, MEM_RELEASE);
}

static void test_rejects_implausible_matches(void)
{
    size_t region = 0;
    unsigned char *mem = load_blob(BLOB_RELEASE, sizeof(BLOB_RELEASE), &region);
    unsigned char before[0x1000];
    patch_report rep;

    CHECK(mem != NULL);
    if (!mem) {
        return;
    }
    /* forward branch instead of a backward one */
    mem[0x100 + 0x34] = 0x10;
    mem[0x100 + 0x35] = 0x00;
    mem[0x100 + 0x36] = 0x00;
    mem[0x100 + 0x37] = 0x00;
    memcpy(before, mem, region);
    CHECK_EQ_U(patch_enum_loop(mem, region, mem_patch_writer, NULL, &rep), 0);
    CHECK_EQ_U(rep.rejected, 1);
    CHECK(memcmp(before, mem, region) == 0);

    /* branch target is not the expected "cmp esi, [ebp+x]" */
    memcpy(mem + 0x100, BLOB_RELEASE, sizeof(BLOB_RELEASE));
    mem[0x100 + 0x15] = 0x90;
    memcpy(before, mem, region);
    CHECK_EQ_U(patch_enum_loop(mem, region, mem_patch_writer, NULL, &rep), 0);
    CHECK_EQ_U(rep.rejected, 1);
    CHECK(memcmp(before, mem, region) == 0);

    /* branch target outside the scanned range */
    memcpy(mem + 0x100, BLOB_RELEASE, sizeof(BLOB_RELEASE));
    CHECK_EQ_U(patch_enum_loop(mem + 0x100 + 0x20, 0x40, mem_patch_writer, NULL, &rep), 0);
    CHECK_EQ_U(rep.rejected, 1);

    /* the conditional branch replaced by something else entirely */
    memcpy(mem + 0x100, BLOB_RELEASE, sizeof(BLOB_RELEASE));
    mem[0x100 + 0x32] = 0x90;
    CHECK_EQ_U(patch_enum_loop(mem, region, mem_patch_writer, NULL, &rep), 0);
    CHECK_EQ_U(rep.rejected, 1);
    VirtualFree(mem, 0, MEM_RELEASE);
}

static void test_no_match_and_empty(void)
{
    unsigned char junk[256];
    patch_report rep;
    size_t i;

    for (i = 0; i < sizeof(junk); ++i) {
        junk[i] = (unsigned char)i;
    }
    CHECK_EQ_U(patch_enum_loop(junk, sizeof(junk), NULL, NULL, &rep), 0);
    CHECK_EQ_U(rep.applied + rep.already + rep.foreign + rep.rejected, 0);
    CHECK(rep.variant == NULL);
    CHECK_EQ_U(patch_enum_loop(NULL, 0, NULL, NULL, &rep), 0);
    CHECK_EQ_U(patch_enum_loop(junk, 0, NULL, NULL, NULL), 0);
}

static void test_both_variants_in_one_image(void)
{
    unsigned char *mem = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    patch_report rep;

    CHECK(mem != NULL);
    if (!mem) {
        return;
    }
    memset(mem, 0xCC, 0x1000);
    memcpy(mem + 0x100, BLOB_RELEASE, sizeof(BLOB_RELEASE));
    memcpy(mem + 0x400, BLOB_DEBUG, sizeof(BLOB_DEBUG));
    memcpy(mem + 0x800, BLOB_RELEASE, sizeof(BLOB_RELEASE));
    memcpy(mem + 0xA00, BLOB_DISP32, sizeof(BLOB_DISP32));
    memcpy(mem + 0xC00, BLOB_EBX, sizeof(BLOB_EBX));
    CHECK_EQ_U(patch_enum_loop(mem, 0x1000, mem_patch_writer, NULL, &rep), 5);
    CHECK_EQ_U(rep.applied, 5);
    CHECK_EQ_U(rep.rejected, 0);
    /* and nothing matches twice: a second pass finds five finished sites */
    CHECK_EQ_U(patch_enum_loop(mem, 0x1000, mem_patch_writer, NULL, &rep), 5);
    CHECK_EQ_U(rep.applied, 0);
    CHECK_EQ_U(rep.already, 5);
    VirtualFree(mem, 0, MEM_RELEASE);
}

void suite_patches(void)
{
    RUN(test_blob_sizes);
    RUN(test_release_variant);
    RUN(test_debug_variant);
    RUN(test_disp32_variant);
    RUN(test_ebx_variant);
    RUN(test_upgrades_third_party_release_fix);
    RUN(test_leaves_third_party_debug_fix);
    RUN(test_rejects_implausible_matches);
    RUN(test_no_match_and_empty);
    RUN(test_both_variants_in_one_image);
}
