/* Helpers for PE images mapped into the current process.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pe_image.h"

#include <string.h>

#include "ini.h"

static const IMAGE_NT_HEADERS32 *nt_headers(HMODULE mod)
{
    const unsigned char *base = (const unsigned char *)mod;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *nt;

    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000) {
        return NULL;
    }
    nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return NULL;
    }
    return nt;
}

int pei_valid(HMODULE mod)
{
    return nt_headers(mod) != NULL;
}

unsigned pei_sections(HMODULE mod, pei_section *out, unsigned max)
{
    const IMAGE_NT_HEADERS32 *nt = nt_headers(mod);
    const IMAGE_SECTION_HEADER *sec;
    unsigned i;

    if (!nt) {
        return 0;
    }
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections && i < max; ++i) {
        out[i].start = (unsigned char *)mod + sec[i].VirtualAddress;
        out[i].size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        out[i].characteristics = sec[i].Characteristics;
        memcpy(out[i].name, sec[i].Name, 8);
        out[i].name[8] = '\0';
    }
    return nt->FileHeader.NumberOfSections;
}

void **pei_find_import(HMODULE mod, const char *dll, const char *func)
{
    const IMAGE_NT_HEADERS32 *nt = nt_headers(mod);
    unsigned char *base = (unsigned char *)mod;
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_IMPORT_DESCRIPTOR *desc;
    DWORD image_size;

    if (!nt || !dll || !func) {
        return NULL;
    }
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return NULL;
    }
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    image_size = nt->OptionalHeader.SizeOfImage;
    if (!dir->VirtualAddress || dir->VirtualAddress >= image_size) {
        return NULL;
    }
    for (desc = (const IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); desc->Name; ++desc) {
        const IMAGE_THUNK_DATA32 *names;
        IMAGE_THUNK_DATA32 *slots;

        if (desc->Name >= image_size || !desc->FirstThunk || !desc->OriginalFirstThunk) {
            continue;
        }
        if (!ini_streq_nocase((const char *)(base + desc->Name), dll)) {
            continue;
        }
        names = (const IMAGE_THUNK_DATA32 *)(base + desc->OriginalFirstThunk);
        slots = (IMAGE_THUNK_DATA32 *)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            const IMAGE_IMPORT_BY_NAME *ibn;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal) || names->u1.AddressOfData >= image_size) {
                continue;
            }
            ibn = (const IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) == 0) {
                return (void **)&slots->u1.Function;
            }
        }
    }
    return NULL;
}

int mem_patch(void *dst, const void *src, size_t len)
{
    DWORD old = 0;
    DWORD ignored = 0;

    if (!dst || !src || !len) {
        return -1;
    }
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old)) {
        return -1;
    }
    memcpy(dst, src, len);
    VirtualProtect(dst, len, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return 0;
}

int mem_patch_writer(void *ctx, unsigned char *dst, const unsigned char *src, size_t len)
{
    (void)ctx;
    return mem_patch(dst, src, len);
}

int pei_hook_import(HMODULE mod, const char *dll, const char *func, void *replacement, void **previous)
{
    void **slot = pei_find_import(mod, dll, func);
    void *old;

    if (!slot) {
        return 0;
    }
    old = *slot;
    if (old == replacement) {
        return 1; /* already hooked */
    }
    if (mem_patch(slot, &replacement, sizeof(replacement)) != 0) {
        return -1;
    }
    if (previous) {
        *previous = old;
    }
    return 1;
}
