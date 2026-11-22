/* PE file helpers: Large Address Aware flag and checksum, on dummy images.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#include "dummy_pe.h"
#include "laa.h"
#include "pe_file.h"
#include "test.h"

/* imagehlp's reference implementation, used to cross-check our checksum. */
typedef PIMAGE_NT_HEADERS(WINAPI *checksum_mapped_fn)(PVOID, DWORD, PDWORD, PDWORD);

static void test_laa_flag(void)
{
    unsigned char pe[DUMMY_PE_SIZE];
    int laa = -1, changed = -1;

    dummy_pe_build(pe, 0x0102, 0);
    CHECK(pef_get_laa(pe, sizeof(pe), &laa) == PEF_OK);
    CHECK(laa == 0);
    CHECK(pef_set_laa(pe, sizeof(pe), 1, &changed) == PEF_OK);
    CHECK(changed == 1);
    CHECK(pef_get_laa(pe, sizeof(pe), &laa) == PEF_OK);
    CHECK(laa == 1);
    CHECK_EQ_U(dummy_pe_characteristics(pe), 0x0122); /* 0x0020 added, nothing else touched */

    /* idempotent */
    CHECK(pef_set_laa(pe, sizeof(pe), 1, &changed) == PEF_OK);
    CHECK(changed == 0);
    CHECK_EQ_U(dummy_pe_characteristics(pe), 0x0122);

    /* and reversible */
    CHECK(pef_set_laa(pe, sizeof(pe), 0, &changed) == PEF_OK);
    CHECK(changed == 1);
    CHECK_EQ_U(dummy_pe_characteristics(pe), 0x0102);
}

static void test_only_flag_bytes_change(void)
{
    unsigned char a[DUMMY_PE_SIZE], b[DUMMY_PE_SIZE];
    size_t i, diffs = 0;

    dummy_pe_build(a, 0x0102, 0); /* checksum 0: must stay 0 */
    memcpy(b, a, sizeof(a));
    CHECK(pef_set_laa(b, sizeof(b), 1, NULL) == PEF_OK);
    for (i = 0; i < sizeof(a); ++i) {
        diffs += a[i] != b[i];
    }
    CHECK_EQ_U(diffs, 1);
}

static void test_checksum(void)
{
    unsigned char pe[DUMMY_PE_SIZE];
    unsigned long ours = 0, stored = 0;
    HMODULE imagehlp = LoadLibraryA("imagehlp.dll");
    checksum_mapped_fn reference =
        imagehlp ? (checksum_mapped_fn)(void *)GetProcAddress(imagehlp, "CheckSumMappedFile") : NULL;

    dummy_pe_build(pe, 0x0102, 1); /* with a valid non-zero checksum */
    CHECK(pef_stored_checksum(pe, sizeof(pe), &stored) == PEF_OK);
    CHECK(stored != 0);
    CHECK(pef_compute_checksum(pe, sizeof(pe), &ours) == PEF_OK);
    CHECK_EQ_U(ours, stored);

    CHECK(reference != NULL);
    if (reference) {
        DWORD header_sum = 0, check_sum = 0;
        CHECK(reference(pe, sizeof(pe), &header_sum, &check_sum) != NULL);
        CHECK_EQ_U(ours, check_sum);

        /* After patching, the stored checksum must be valid again. */
        CHECK(pef_set_laa(pe, sizeof(pe), 1, NULL) == PEF_OK);
        CHECK(reference(pe, sizeof(pe), &header_sum, &check_sum) != NULL);
        CHECK_EQ_U(header_sum, check_sum);
        CHECK(check_sum != stored);

        /* Odd file length. The buffer carries one extra zero byte because
         * Wine's CheckSumMappedFile reads a whole 16-bit word at the end of an
         * odd-sized file, i.e. one byte past the length it was given. */
        {
            unsigned char odd[DUMMY_PE_SIZE + 2];
            memcpy(odd, pe, DUMMY_PE_SIZE);
            odd[DUMMY_PE_SIZE] = 0x5A;
            odd[DUMMY_PE_SIZE + 1] = 0;
            CHECK(pef_compute_checksum(odd, DUMMY_PE_SIZE + 1, &ours) == PEF_OK);
            CHECK(reference(odd, DUMMY_PE_SIZE + 1, &header_sum, &check_sum) != NULL);
            CHECK_EQ_U(ours, check_sum);
        }
    }
}

static void test_rejects_bad_input(void)
{
    unsigned char pe[DUMMY_PE_SIZE];
    int laa = 0;

    CHECK(pef_get_laa(NULL, 0, &laa) == PEF_NOT_PE);
    CHECK(pef_get_laa((const unsigned char *)"MZ", 2, &laa) == PEF_TRUNCATED);
    CHECK(pef_get_laa((const unsigned char *)"not a pe file at all, just some text.......................................",
                      64, &laa) == PEF_NOT_PE);

    dummy_pe_build(pe, 0x0102, 0);
    pe[0x80] = 'X'; /* break the PE signature */
    CHECK(pef_set_laa(pe, sizeof(pe), 1, NULL) == PEF_NOT_PE);

    dummy_pe_build(pe, 0x0102, 0);
    pe[0x84] = 0x64;
    pe[0x85] = 0x86; /* machine = AMD64 */
    CHECK(pef_set_laa(pe, sizeof(pe), 1, NULL) == PEF_NOT_X86);

    dummy_pe_build(pe, 0x0102, 0);
    pe[0x98] = 0x0B;
    pe[0x99] = 0x02; /* PE32+ magic */
    CHECK(pef_get_laa(pe, sizeof(pe), &laa) == PEF_NOT_X86);

    dummy_pe_build(pe, 0x0102, 0);
    pe[0x3C] = 0xF0;
    pe[0x3D] = 0xFF;
    pe[0x3E] = 0xFF;
    pe[0x3F] = 0x7F; /* e_lfanew far outside the file */
    CHECK(pef_get_laa(pe, sizeof(pe), &laa) == PEF_TRUNCATED);

    dummy_pe_build(pe, 0x0102, 0);
    CHECK(pef_get_laa(pe, 0x90, &laa) == PEF_TRUNCATED); /* file cut inside the headers */
    CHECK_STR(pef_status_text(PEF_OK), "ok");
}

/* End-to-end: the DLL's own file patcher on a real file, including the
 * timestamped backup that the uninstallers look for. */
static void test_file_patcher(void)
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH], pattern[MAX_PATH], bak[MAX_PATH];
    unsigned char pe[DUMMY_PE_SIZE], back[DUMMY_PE_SIZE];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    DWORD n = 0;
    int laa = 0;
    int backups = 0;

    CHECK(GetTempPathW(MAX_PATH, dir) > 0);
    _snwprintf(exe, MAX_PATH, L"%lsfc3pp-unit-%lu.exe", dir, (unsigned long)GetCurrentProcessId());
    _snwprintf(pattern, MAX_PATH, L"%ls.fc3pp-*.bak", exe);
    dummy_pe_build(pe, 0x0102, 1);

    h = CreateFileW(exe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    CHECK(WriteFile(h, pe, sizeof(pe), &n, NULL) && n == sizeof(pe));
    CloseHandle(h);

    CHECK(laa_apply_to_file(exe) == LAA_PATCHED);
    CHECK(laa_apply_to_file(exe) == LAA_ALREADY); /* second run changes nothing */

    h = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(ReadFile(h, back, sizeof(back), &n, NULL) && n == sizeof(back));
        CloseHandle(h);
        CHECK(pef_get_laa(back, sizeof(back), &laa) == PEF_OK && laa == 1);
    }

    /* Exactly one backup, byte-identical to the original. */
    h = FindFirstFileW(pattern, &fd);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            HANDLE b;
            ++backups;
            _snwprintf(bak, MAX_PATH, L"%ls%ls", dir, fd.cFileName);
            b = CreateFileW(bak, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            CHECK(b != INVALID_HANDLE_VALUE);
            if (b != INVALID_HANDLE_VALUE) {
                CHECK(ReadFile(b, back, sizeof(back), &n, NULL) && n == sizeof(back));
                CloseHandle(b);
                CHECK(memcmp(back, pe, sizeof(pe)) == 0);
            }
            DeleteFileW(bak);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    CHECK(backups == 1);
    DeleteFileW(exe);

    /* Non-PE and missing files are refused without side effects. */
    CHECK(laa_apply_to_file(exe) == LAA_IO_ERROR);
    h = CreateFileW(exe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, "plain text, not an executable", 29, &n, NULL);
        CloseHandle(h);
        CHECK(laa_apply_to_file(exe) == LAA_NOT_PE);
        CHECK(FindFirstFileW(pattern, &fd) == INVALID_HANDLE_VALUE);
        DeleteFileW(exe);
    }
}

void suite_pe_file(void)
{
    RUN(test_laa_flag);
    RUN(test_only_flag_bytes_change);
    RUN(test_checksum);
    RUN(test_rejects_bad_input);
    RUN(test_file_patcher);
}
