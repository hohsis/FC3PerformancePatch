/* Draw-distance presets: the copy of the profile the game gets to read.
 *
 * The engine opens files on disk through its imports of CreateFileW and
 * CreateFileA. When one of them opens "GamerProfile.xml" for reading, the patch
 *   1. reads the real file,
 *   2. reads engine\settings\DefaultRenderConfig.xml from the game's archives
 *      (patch.fat first, so a mod's values are the ones that get scaled),
 *   3. writes the profile with the preset's sections added to a temporary
 *      file and opens that instead, with the caller's own parameters.
 * The temporary file deletes itself when the game closes it. The first time
 * this happens the real profile is copied next to itself, once. Opens for
 * writing are not touched: the game saves its profile to the real file, and
 * it does not write such sections out, so nothing of this ever lands there.
 * If any step fails the game simply gets its real file.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gfx_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dunia.h"
#include "gfx_preset.h"
#include "log.h"
#include "pe_image.h"

#define MAX_PROFILE_BYTES (1u << 20)
#define MAX_CONFIG_BYTES (1u << 20)
#define MAX_FAT_BYTES (64u << 20)
#define PATH_CHARS (MAX_PATH * 2)

typedef HANDLE(WINAPI *create_file_w_fn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *create_file_a_fn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static create_file_w_fn g_create_w;
static create_file_a_fn g_create_a;
static int g_level;
static volatile LONG g_opens, g_applied, g_failed, g_logged, g_backed_up;
static SRWLOCK g_lock = SRWLOCK_INIT;
static char *g_config; /* the default render configuration, read once */
static size_t g_config_len;
static int g_config_tried;
static unsigned long g_copies; /* numbers the temporary files */

static const char *level_name(int level)
{
    return level == GP_VERY_FAR ? "Very far" : "Far";
}

static int is_profile_w(const wchar_t *path)
{
    static const wchar_t name[] = L"GamerProfile.xml";
    size_t n, k = sizeof(name) / sizeof(name[0]) - 1;

    if (!path) {
        return 0;
    }
    n = wcslen(path);
    return n >= k && _wcsicmp(path + n - k, name) == 0 && (n == k || path[n - k - 1] == L'\\' || path[n - k - 1] == L'/');
}

/* Reads a whole file of at most `limit` bytes. Caller frees. */
static char *read_whole(const wchar_t *path, size_t limit, size_t *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    char *buf = NULL;
    DWORD got = 0;

    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && (ULONGLONG)size.QuadPart <= limit) {
        buf = (char *)malloc((size_t)size.QuadPart);
        if (buf && (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart)) {
            free(buf);
            buf = NULL;
        }
        *len = (size_t)size.QuadPart;
    }
    CloseHandle(h);
    return buf;
}

/* One file out of "<data>\<archive>.fat/.dat", decompressed. Caller frees. */
static char *read_from_archive(const wchar_t *data_dir, const wchar_t *archive, const char *path_in_archive, size_t *len)
{
    wchar_t fat_path[PATH_CHARS], dat_path[PATH_CHARS];
    unsigned char *index, *stored = NULL, *content = NULL;
    size_t index_len = 0;
    dn_fat fat;
    dn_entry e;
    HANDLE dat;
    LARGE_INTEGER pos;
    DWORD got = 0;

    if (_snwprintf(fat_path, PATH_CHARS, L"%ls\\%ls.fat", data_dir, archive) < 0 ||
        _snwprintf(dat_path, PATH_CHARS, L"%ls\\%ls.dat", data_dir, archive) < 0) {
        return NULL;
    }
    fat_path[PATH_CHARS - 1] = dat_path[PATH_CHARS - 1] = L'\0';
    index = (unsigned char *)read_whole(fat_path, MAX_FAT_BYTES, &index_len);
    if (!index) {
        return NULL;
    }
    if (dn_fat_open(&fat, index, index_len) != 0 || !dn_fat_find(&fat, dn_name_hash(path_in_archive), &e) ||
        !e.stored || e.stored > MAX_CONFIG_BYTES || e.size > MAX_CONFIG_BYTES) {
        free(index);
        return NULL;
    }
    free(index);
    dat = CreateFileW(dat_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dat == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    stored = (unsigned char *)malloc(e.stored);
    pos.QuadPart = (LONGLONG)e.offset;
    if (stored && SetFilePointerEx(dat, pos, NULL, FILE_BEGIN) && ReadFile(dat, stored, e.stored, &got, NULL) &&
        got == e.stored) {
        if (e.scheme == DN_SCHEME_STORED) {
            content = stored;
            stored = NULL;
            *len = e.stored;
        } else if (e.scheme == DN_SCHEME_LZO) {
            content = (unsigned char *)malloc(e.size ? e.size : 1);
            if (content) {
                long n = dn_lzo1x(stored, e.stored, content, e.size);
                if (n != (long)e.size) {
                    free(content);
                    content = NULL;
                } else {
                    *len = (size_t)n;
                }
            }
        }
    }
    free(stored);
    CloseHandle(dat);
    return (char *)content;
}

char *gfx_profile_read_default_config(const wchar_t *data_dir, size_t *len)
{
    static const char path[] = "engine\\settings\\defaultrenderconfig.xml";
    char *config = read_from_archive(data_dir, L"patch", path, len);

    return config ? config : read_from_archive(data_dir, L"common", path, len);
}

/* The game's default render configuration; NULL when it cannot be read. */
static const char *default_config(size_t *len)
{
    if (!g_config_tried) {
        wchar_t dir[PATH_CHARS];
        wchar_t *slash = NULL;

        g_config_tried = 1;
        /* "<game>\bin\<exe>"  ->  "<game>\data_win32" */
        if (GetModuleFileNameW(NULL, dir, MAX_PATH)) {
            dir[MAX_PATH - 1] = L'\0';
            slash = wcsrchr(dir, L'\\');
            if (slash) {
                *slash = L'\0';
                slash = wcsrchr(dir, L'\\');
            }
        }
        if (slash) {
            wcscpy(slash, L"\\data_win32");
            g_config = gfx_profile_read_default_config(dir, &g_config_len);
        }
    }
    *len = g_config_len;
    return g_config;
}

/* Why a profile was handed to the game as it is. */
enum {
    OV_NO_PROFILE = 0,    /* there is none (yet), or it cannot be read: nothing to say */
    OV_NO_CONFIG = -1,    /* the default render configuration could not be read */
    OV_NOTHING_TO_ADD = -2, /* the profile has the sections already, or is not in the known form */
    OV_NO_COPY = -3       /* the temporary copy could not be written or opened */
};

/* Writes the profile with the preset to a temporary file and returns its
 * path in temp_path. Returns the number of sections added, or an OV_ value. */
static int write_overlay(const wchar_t *profile_path, wchar_t *temp_path)
{
    size_t profile_len = 0, config_len = 0, cap;
    char *profile, *out;
    const char *config;
    unsigned sections = 0;
    long n;
    wchar_t temp_dir[MAX_PATH];
    HANDLE h;
    DWORD written = 0;
    int ok = 0;

    profile = read_whole(profile_path, MAX_PROFILE_BYTES, &profile_len);
    if (!profile) {
        return OV_NO_PROFILE;
    }
    config = default_config(&config_len);
    if (!config) {
        free(profile);
        return OV_NO_CONFIG;
    }
    cap = profile_len + config_len + GP_OVERLAY_SLACK;
    out = (char *)malloc(cap);
    n = out ? gp_overlay(profile, profile_len, config, config_len, (gp_level)g_level, out, cap, &sections) : -1;
    free(profile);
    if (n <= 0) {
        free(out);
        return out ? OV_NOTHING_TO_ADD : OV_NO_COPY;
    }
    if (!GetTempPathW(MAX_PATH, temp_dir) ||
        _snwprintf(temp_path, PATH_CHARS, L"%lsfc3pp-profile-%lu-%lu.xml", temp_dir,
                   (unsigned long)GetCurrentProcessId(), (unsigned long)++g_copies) < 0) {
        free(out);
        return OV_NO_COPY;
    }
    temp_path[PATH_CHARS - 1] = L'\0';
    h = CreateFileW(temp_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ok = WriteFile(h, out, (DWORD)n, &written, NULL) && written == (DWORD)n;
        CloseHandle(h);
        if (!ok) {
            DeleteFileW(temp_path);
        }
    }
    free(out);
    return ok ? (int)sections : OV_NO_COPY;
}

static void report(int result)
{
    if (result > 0) {
        InterlockedIncrement(&g_applied);
        if (InterlockedExchange(&g_logged, 1) == 0) {
            LOGI("graphics: draw distance '%s': %d sections with longer distances added to the profile as the game "
                 "reads it (the file on disk is not changed)",
                 level_name(g_level), result);
        }
    } else if (result != OV_NO_PROFILE && InterlockedIncrement(&g_failed) == 1) {
        if (result == OV_NOTHING_TO_ADD) {
            LOGI("graphics: the draw-distance preset adds nothing: the profile already has its own Geometry, Shadow "
                 "and Terrain sections, or is not in the form this patch knows; the game reads it as it is");
        } else if (result == OV_NO_CONFIG) {
            LOGE("graphics: the draw-distance preset cannot be applied: the game's default render configuration "
                 "could not be read from its archives; the game uses its normal settings");
        } else {
            LOGE("graphics: the draw-distance preset cannot be applied: a temporary copy of the profile could not "
                 "be written (error %lu); the game uses its normal settings",
                 (unsigned long)GetLastError());
        }
    }
}

/* Insurance for a feature that hands the game something other than its file:
 * the first time a preset is applied, the profile as it is on disk is copied
 * next to itself. An existing copy is never overwritten - if the game should
 * ever take a preset badly and rewrite its profile, the copy must not follow. */
static void keep_backup(const wchar_t *profile_path)
{
    wchar_t backup[PATH_CHARS];

    if (_snwprintf(backup, PATH_CHARS, L"%ls" GFX_PROFILE_BACKUP_SUFFIX, profile_path) < 0) {
        return;
    }
    backup[PATH_CHARS - 1] = L'\0';
    if (CopyFileW(profile_path, backup, TRUE)) {
        LOGI("graphics: the profile as it is on disk was copied next to itself (GamerProfile.xml%ls); the copy "
             "is made once and never overwritten",
             GFX_PROFILE_BACKUP_SUFFIX);
    } else if (GetLastError() != ERROR_FILE_EXISTS) {
        LOGD("graphics: no copy of the profile could be made (error %lu)", (unsigned long)GetLastError());
    }
}

/* The game opens `path` to read it. Returns a handle to a copy with the
 * preset's sections, opened the way the game asked for, or
 * INVALID_HANDLE_VALUE when the game is to get its own file. */
static HANDLE open_overlay(const wchar_t *path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                           DWORD flags, HANDLE template_file)
{
    wchar_t temp_path[PATH_CHARS];
    HANDLE h = INVALID_HANDLE_VALUE;
    int result;

    InterlockedIncrement(&g_opens);
    AcquireSRWLockExclusive(&g_lock);
    result = write_overlay(path, temp_path);
    ReleaseSRWLockExclusive(&g_lock);
    if (result > 0) {
        /* The copy removes itself when the game closes it. */
        h = CreateFileW(temp_path, access, share | FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, security,
                        OPEN_EXISTING, flags | FILE_FLAG_DELETE_ON_CLOSE, template_file);
        if (h == INVALID_HANDLE_VALUE) {
            DeleteFileW(temp_path);
            result = OV_NO_COPY;
        } else if (InterlockedExchange(&g_backed_up, 1) == 0) {
            keep_backup(path);
        }
    }
    report(result);
    return h;
}

/* An open that only reads an existing file. The engine's file class asks for
 * the specific rights (FILE_READ_DATA to read; FILE_WRITE_DATA or
 * FILE_APPEND_DATA with CREATE_ALWAYS or OPEN_ALWAYS to write), not for the
 * generic ones. */
static int wants_overlay(DWORD access, DWORD disposition)
{
    const DWORD changing = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                           FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER;

    return g_level > GP_ORIGINAL && !(access & changing) && disposition == OPEN_EXISTING;
}

static HANDLE WINAPI hook_create_file_w(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                        DWORD disposition, DWORD flags, HANDLE template_file)
{
    if (wants_overlay(access, disposition) && is_profile_w(path)) {
        HANDLE h = open_overlay(path, access, share, security, flags, template_file);
        if (h != INVALID_HANDLE_VALUE) {
            return h;
        }
    }
    return g_create_w(path, access, share, security, disposition, flags, template_file);
}

static HANDLE WINAPI hook_create_file_a(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                        DWORD disposition, DWORD flags, HANDLE template_file)
{
    wchar_t wide[PATH_CHARS];

    if (wants_overlay(access, disposition) && path && MultiByteToWideChar(CP_ACP, 0, path, -1, wide, PATH_CHARS) > 0 &&
        is_profile_w(wide)) {
        HANDLE h = open_overlay(wide, access, share, security, flags, template_file);
        if (h != INVALID_HANDLE_VALUE) {
            return h;
        }
    }
    return g_create_a(path, access, share, security, disposition, flags, template_file);
}

static unsigned hook(HMODULE engine, const char *dll, const char *name, void *replacement, void *original_out)
{
    void **slot = pei_find_import(engine, dll, name);
    void *previous = NULL;

    if (!slot) {
        return 0;
    }
    if (*slot == replacement) {
        return 1; /* already in place */
    }
    if (pei_hook_import(engine, dll, name, replacement, &previous) != 1 || !previous) {
        return 0;
    }
    memcpy(original_out, &previous, sizeof(previous));
    return 1;
}

unsigned gfx_profile_install(HMODULE engine, int level)
{
    unsigned hooks = 0;

    if (level <= GP_ORIGINAL || level >= GP_LEVELS) {
        return 0;
    }
    g_level = level;
    hooks += hook(engine, "kernel32.dll", "CreateFileW", (void *)hook_create_file_w, &g_create_w);
    hooks += hook(engine, "kernel32.dll", "CreateFileA", (void *)hook_create_file_a, &g_create_a);
    if (hooks) {
        LOGI("graphics: draw distance '%s' selected; it is applied when the game reads its profile", level_name(level));
    } else {
        LOGE("graphics: the draw-distance preset is not available: the engine's file calls were not found");
        g_level = GP_ORIGINAL;
    }
    return hooks;
}

void gfx_profile_log_stats(void)
{
    if (g_level <= GP_ORIGINAL) {
        return;
    }
    if (!g_opens) {
        LOGI("graphics: the draw-distance preset was not applied: the game did not open its profile through a "
             "file call this patch watches");
    } else if (g_applied) {
        LOGI("graphics: the profile was handed to the game with the draw-distance preset %ld time%s", (long)g_applied,
             g_applied == 1 ? "" : "s");
    }
}
