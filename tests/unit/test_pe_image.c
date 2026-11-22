/* In-memory PE helpers: section walk, import lookup and import hooking,
 * exercised on the test executable itself.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#include "pe_image.h"
#include "test.h"

static DWORD WINAPI fake_tick_count(void)
{
    return 0x12345678;
}

/* The optimiser may keep an import address in a register across calls within
 * one function. Going through a non-inlined helper makes every call re-read
 * the import table, like separate engine functions do. */
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static DWORD call_tick_count(void)
{
    return GetTickCount();
}

static void test_sections(void)
{
    HMODULE self = GetModuleHandleW(NULL);
    pei_section sec[32];
    unsigned n, i;
    int executable = 0;

    CHECK(pei_valid(self));
    CHECK(!pei_valid(NULL));
    n = pei_sections(self, sec, 32);
    CHECK(n >= 2 && n <= 32);
    for (i = 0; i < n && i < 32; ++i) {
        CHECK(sec[i].start > (unsigned char *)self);
        CHECK(sec[i].size > 0);
        if (sec[i].characteristics & IMAGE_SCN_MEM_EXECUTE) {
            executable = 1;
            /* this very function lives in an executable section */
        }
    }
    CHECK(executable);
    {
        /* Not a PE image at all. */
        static const char text[256] = "MZ this is not really an image";
        CHECK(!pei_valid((HMODULE)(void *)text));
        CHECK_EQ_U(pei_sections((HMODULE)(void *)text, sec, 32), 0);
    }
}

static void test_import_lookup_and_hook(void)
{
    HMODULE self = GetModuleHandleW(NULL);
    void **slot;
    void *previous = NULL;
    (void)call_tick_count(); /* guarantees the import exists */
    slot = pei_find_import(self, "kernel32.dll", "GetTickCount"); /* DLL name is case-insensitive */
    CHECK(slot != NULL);
    CHECK(pei_find_import(self, "KERNEL32.dll", "GetTickCount") == slot);
    CHECK(pei_find_import(self, "KERNEL32.dll", "gettickcount") == NULL); /* function names are exact */
    CHECK(pei_find_import(self, "KERNEL32.dll", "NoSuchFunctionAnywhere") == NULL);
    CHECK(pei_find_import(self, "nosuchmodule.dll", "GetTickCount") == NULL);
    CHECK(pei_find_import(self, NULL, "GetTickCount") == NULL);

    CHECK(pei_hook_import(self, "KERNEL32.dll", "NoSuchFunctionAnywhere", (void *)fake_tick_count, NULL) == 0);

    CHECK(pei_hook_import(self, "KERNEL32.dll", "GetTickCount", (void *)fake_tick_count, &previous) == 1);
    CHECK(previous != NULL && previous != (void *)fake_tick_count);
    CHECK_EQ_U(call_tick_count(), 0x12345678); /* calls through the import table now reach the hook */

    /* hooking twice is harmless and does not lose the original */
    {
        void *again = NULL;
        CHECK(pei_hook_import(self, "KERNEL32.dll", "GetTickCount", (void *)fake_tick_count, &again) == 1);
        CHECK(again == NULL);
    }

    /* restore */
    CHECK(pei_hook_import(self, "KERNEL32.dll", "GetTickCount", previous, NULL) == 1);
    CHECK(call_tick_count() != 0x12345678);
}

static void test_mem_patch_readonly(void)
{
    unsigned char *page = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    static const unsigned char data[4] = {1, 2, 3, 4};
    DWORD old = 0;
    MEMORY_BASIC_INFORMATION mbi;

    CHECK(page != NULL);
    if (!page) {
        return;
    }
    memset(page, 0xEE, 0x1000);
    CHECK(VirtualProtect(page, 0x1000, PAGE_READONLY, &old));
    CHECK(mem_patch(page + 0x10, data, sizeof(data)) == 0);
    CHECK(memcmp(page + 0x10, data, sizeof(data)) == 0);
    CHECK_EQ_U(page[0x0F], 0xEE);
    CHECK_EQ_U(page[0x14], 0xEE);
    /* the original protection is restored */
    CHECK(VirtualQuery(page, &mbi, sizeof(mbi)) == sizeof(mbi));
    CHECK_EQ_U(mbi.Protect, PAGE_READONLY);
    CHECK(mem_patch(NULL, data, 4) == -1);
    CHECK(mem_patch(page, NULL, 4) == -1);
    CHECK(mem_patch(page, data, 0) == -1);
    VirtualFree(page, 0, MEM_RELEASE);
}

void suite_pe_image(void)
{
    RUN(test_sections);
    RUN(test_import_lookup_and_hook);
    RUN(test_mem_patch_readonly);
}
