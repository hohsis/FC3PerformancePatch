/* Runs the archive scan on the game's data folder (see tex_stale.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tex_stale.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "log.h"

#define MAX_ARCHIVES 128
#define MAX_DEPTH 4
#define MAX_FAT_BYTES (64u << 20)
#define MAX_STALE 4096
#define FIRST_WAIT_MS 5000
/* The scan starts this long after the engine was found, or at once when its
 * result is first asked for. The game starts twice: the first process only
 * hands over to the launcher and is gone within a second (longer under an
 * emulator), and it should not read forty archive indexes for nothing. More
 * than that: under Wine with the FEX emulator a process that ends in the
 * moment in which one of its threads finishes hangs now and then (measured:
 * 5 of 150 runs of a test whose scan ended 1 ms before the process, none of
 * 150 with 200 ms in between). A thread that is still waiting is no such
 * risk. */
#define START_DELAY_MS 10000
#define PATH_CHARS (MAX_PATH * 2)

typedef struct archive {
    unsigned char *index; /* the .fat file */
    HANDLE data;          /* the .dat file */
} archive;

static archive g_archives[MAX_ARCHIVES];
static dn_fat g_fats[MAX_ARCHIVES];
static unsigned g_archive_count;

static unsigned long long g_stale[MAX_STALE];
static volatile LONG g_stale_count; /* valid once g_done is signalled */
#define MAX_SHARED 1024
static unsigned long long g_shared_names[MAX_SHARED];   /* top level files several textures name */
static unsigned long long g_shared_ids[MAX_SHARED * 2]; /* the same files by content */
static volatile LONG g_shared_name_count, g_shared_id_count;
static volatile LONG g_shared_handled, g_want_replaced;
static HANDLE g_done;
static HANDLE g_needed; /* set when somebody asks for the result */
static volatile LONG g_started;
static volatile LONG g_waited;

static int read_data(void *ctx, unsigned index, unsigned long long offset, void *buf, size_t len)
{
    LARGE_INTEGER pos;
    DWORD got = 0;

    (void)ctx;
    if (index >= g_archive_count || g_archives[index].data == INVALID_HANDLE_VALUE) {
        return -1;
    }
    pos.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(g_archives[index].data, pos, NULL, FILE_BEGIN) ||
        !ReadFile(g_archives[index].data, buf, (DWORD)len, &got, NULL) || got != len) {
        return -1;
    }
    return 0;
}

static HANDLE open_shared(const wchar_t *path)
{
    /* The game has these files open; share everything. */
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

/* Adds "<name>.fat" with its "<name>.dat" to the list. An archive without
 * data (the game ships several empty ones) is skipped. */
static void add_archive(const wchar_t *fat_path)
{
    wchar_t dat_path[PATH_CHARS];
    size_t len = wcslen(fat_path);
    HANDLE fat, dat;
    LARGE_INTEGER size;
    unsigned char *index = NULL;
    size_t index_size = 0;
    DWORD got = 0;

    if (g_archive_count >= MAX_ARCHIVES || len < 4 || len >= PATH_CHARS) {
        return;
    }
    memcpy(dat_path, fat_path, (len + 1) * sizeof(wchar_t));
    memcpy(dat_path + len - 3, L"dat", 3 * sizeof(wchar_t));

    dat = open_shared(dat_path);
    if (dat == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!GetFileSizeEx(dat, &size) || size.QuadPart == 0) {
        CloseHandle(dat);
        return;
    }
    fat = open_shared(fat_path);
    if (fat != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(fat, &size) && size.QuadPart > 0 && size.QuadPart <= (LONGLONG)MAX_FAT_BYTES) {
            index_size = (size_t)size.QuadPart;
            index = (unsigned char *)malloc(index_size);
            if (index && (!ReadFile(fat, index, (DWORD)index_size, &got, NULL) || got != (DWORD)index_size)) {
                free(index);
                index = NULL;
            }
        }
        CloseHandle(fat);
    }
    if (!index || dn_fat_open(&g_fats[g_archive_count], index, index_size) != 0) {
        free(index);
        CloseHandle(dat);
        return;
    }
    g_archives[g_archive_count].index = index;
    g_archives[g_archive_count].data = dat;
    g_archive_count++;
}

static int has_suffix(const wchar_t *name, const wchar_t *suffix)
{
    size_t n = wcslen(name), s = wcslen(suffix);
    return n >= s && _wcsicmp(name + n - s, suffix) == 0;
}

static int join(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
    int n = _snwprintf(out, PATH_CHARS, L"%ls\\%ls", dir, name);
    out[PATH_CHARS - 1] = L'\0';
    return n > 0 && n < PATH_CHARS;
}

/* Every "*.fat" below dir except the override archive itself. */
static void add_game_archives(const wchar_t *dir, const wchar_t *override_path, int depth)
{
    wchar_t pattern[PATH_CHARS];
    WIN32_FIND_DATAW fd;
    HANDLE find;

    if (!join(pattern, dir, L"*")) {
        return;
    }
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        wchar_t path[PATH_CHARS];
        if (fd.cFileName[0] == L'.' || !join(path, dir, fd.cFileName)) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < MAX_DEPTH) {
                add_game_archives(path, override_path, depth + 1);
            }
        } else if (has_suffix(fd.cFileName, L".fat") && _wcsicmp(path, override_path) != 0) {
            add_archive(path);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

/* Opens the archives of data_dir. Returns 1 when there is an override archive. */
static int open_archives(const wchar_t *data_dir, ts_input *input)
{
    wchar_t override_path[PATH_CHARS];

    g_archive_count = 0;
    if (!data_dir || !join(override_path, data_dir, L"patch.fat")) {
        return 0;
    }
    add_archive(override_path);
    if (g_archive_count != 1) {
        return 0;
    }
    add_game_archives(data_dir, override_path, 0);
    input->fats = g_fats;
    input->count = g_archive_count;
    input->read = read_data;
    input->ctx = NULL;
    return 1;
}

static void close_archives(void)
{
    unsigned i;

    for (i = 0; i < g_archive_count; ++i) {
        CloseHandle(g_archives[i].data);
        free(g_archives[i].index);
        g_archives[i].index = NULL;
    }
    g_archive_count = 0;
}

long tex_stale_scan_folder(const wchar_t *data_dir, unsigned long long *ids, size_t cap, ts_stats *stats,
                           unsigned *archives)
{
    ts_input input;
    ts_stats local;
    long count = 0;

    if (!stats) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));
    if (open_archives(data_dir, &input)) {
        count = tex_scan_stale(&input, ids, cap, stats);
    }
    if (archives) {
        *archives = g_archive_count;
    }
    close_archives();
    return count;
}

long tex_stale_scan_folder_shared(const wchar_t *data_dir, unsigned long long *names, size_t name_cap,
                                  unsigned long long *ids, size_t id_cap, size_t *id_count, ts_shared_stats *stats)
{
    ts_input input;
    long count = 0;

    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
    if (id_count) {
        *id_count = 0;
    }
    if (open_archives(data_dir, &input)) {
        count = tex_scan_shared(&input, names, name_cap, ids, id_cap, id_count, stats);
    }
    close_archives();
    return count;
}

static DWORD WINAPI scan_thread(LPVOID unused)
{
    wchar_t dir[PATH_CHARS];
    wchar_t *slash = NULL;
    ts_stats stats;
    ts_shared_stats shared;
    DWORD started = GetTickCount();
    long count = 0, n_shared = 0;
    size_t shared_ids = 0;
    unsigned archives = 0;

    (void)unused;
    memset(&stats, 0, sizeof(stats));
    if (g_needed) {
        WaitForSingleObject(g_needed, START_DELAY_MS);
        started = GetTickCount();
    }
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
        if (g_want_replaced) {
            count = tex_stale_scan_folder(dir, g_stale, MAX_STALE, &stats, &archives);
        }
        n_shared = tex_stale_scan_folder_shared(dir, g_shared_names, MAX_SHARED, g_shared_ids, MAX_SHARED * 2,
                                                &shared_ids, &shared);
    } else {
        memset(&shared, 0, sizeof(shared));
    }
    if (n_shared < 0) {
        LOGE("textures: not enough memory to look for top mip level files that several textures share");
        n_shared = 0;
        shared_ids = 0;
    }
    if (count < 0) {
        LOGE("textures: not enough memory to check the patch archive for replaced textures");
        count = 0;
    }
    if (!g_want_replaced) {
        /* only the shared files were asked for */
    } else if (stats.linked == stats.own_top) {
        LOGD("textures: the patch archive replaces no texture that has a separate top mip level "
             "(%u archives read in %lu ms)",
             archives, (unsigned long)(GetTickCount() - started));
    } else {
        LOGI("textures: the patch archive holds %u textures with a separate top mip level; %u bring their own, "
             "%u would get an original one of the wrong size, %u an original one made for the texture they "
             "replace (those %u files will be left out, %u are intact) - %u archives read in %lu ms",
             stats.linked, stats.own_top, stats.unfit, stats.stale, (unsigned)count, stats.unchanged, archives,
             (unsigned long)(GetTickCount() - started));
    }
    if (stats.truncated) {
        LOGE("textures: more replaced textures than this patch keeps track of; %u original top mip levels "
             "may still show up close",
             stats.truncated);
    }
    if (n_shared > 0) {
        LOGI("textures: %ld top mip level files are named by more than one texture (%u by several textures of the "
             "patch archive, %u by one of them and one of the game). The game is built for one texture per "
             "file; %s",
             n_shared, shared.among_override, shared.with_game,
             g_shared_handled ? "these files will not be loaded"
                              : "with [Textures] SkipSharedMip0=0 it loads them and may crash when it unloads them");
    }
    if (shared.truncated) {
        LOGE("textures: more shared top mip level files than this patch keeps track of (%u left unlisted)",
             shared.truncated);
    }
    g_shared_id_count = (LONG)shared_ids;
    InterlockedExchange(&g_shared_name_count, (LONG)n_shared);
    InterlockedExchange(&g_stale_count, (LONG)count);
    SetEvent(g_done);
    return 0;
}

void tex_stale_start(void)
{
    HANDLE thread;

    if (!g_want_replaced && !g_shared_handled) {
        return; /* nobody asked */
    }
    if (InterlockedExchange(&g_started, 1)) {
        return;
    }
    g_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_done) {
        return;
    }
    g_needed = CreateEventW(NULL, TRUE, FALSE, NULL); /* without it the scan simply starts at once */
    thread = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        SetEvent(g_done); /* nothing will ever be listed */
    }
}

int tex_stale_check(const void *buffer, const void *dds, unsigned dds_size)
{
    const unsigned char *xbt = (const unsigned char *)buffer;
    const unsigned char *image = (const unsigned char *)dds;
    unsigned long header;

    if (!g_done || !xbt || !image) {
        return 0;
    }
    /* The engine streams its first top level long after start-up; if the scan
     * is still running then (slow disk), give it a moment, once. */
    if (g_needed) {
        SetEvent(g_needed);
    }
    if (WaitForSingleObject(g_done, InterlockedExchange(&g_waited, 1) ? 0 : FIRST_WAIT_MS) != WAIT_OBJECT_0) {
        return 0;
    }
    if (g_stale_count <= 0) {
        return 0;
    }
    /* buffer must be the texture file the image sits in. */
    if (image <= xbt || (size_t)(image - xbt) > DN_XBT_FIXED_HEADER + DN_XBT_MAX_PATH || memcmp(xbt, "TBX\0", 4) != 0) {
        return 0;
    }
    header = (unsigned long)xbt[8] | ((unsigned long)xbt[9] << 8) | ((unsigned long)xbt[10] << 16) |
             ((unsigned long)xbt[11] << 24);
    if (header != (unsigned long)(image - xbt)) {
        return 0;
    }
    return tex_scan_contains(g_stale, (size_t)g_stale_count, dn_top_identity(xbt, image, dds_size, dds_size));
}

/* Has the scan finished? Waits a moment the first time something asks. */
static int scan_done(void)
{
    if (!g_done) {
        return 0;
    }
    if (g_needed) {
        SetEvent(g_needed);
    }
    return WaitForSingleObject(g_done, InterlockedExchange(&g_waited, 1) ? 0 : FIRST_WAIT_MS) == WAIT_OBJECT_0;
}

void tex_stale_shared_handled(void)
{
    InterlockedExchange(&g_shared_handled, 1);
}

void tex_stale_want_replaced(void)
{
    InterlockedExchange(&g_want_replaced, 1);
}

long tex_stale_shared_count(void)
{
    return g_done && WaitForSingleObject(g_done, 0) == WAIT_OBJECT_0 ? (long)g_shared_name_count : -1;
}

int tex_stale_shared_name(unsigned long long name_hash)
{
    if (!scan_done() || g_shared_name_count <= 0) {
        return 0;
    }
    return tex_scan_contains(g_shared_names, (size_t)g_shared_name_count, name_hash);
}

int tex_stale_shared_file(const void *file, unsigned size)
{
    const unsigned char *xbt = (const unsigned char *)file;
    unsigned long header;

    if (!scan_done() || g_shared_id_count <= 0 || !xbt || size < DN_XBT_FIXED_HEADER + 128 ||
        memcmp(xbt, "TBX\0", 4) != 0) {
        return 0;
    }
    header = (unsigned long)xbt[8] | ((unsigned long)xbt[9] << 8) | ((unsigned long)xbt[10] << 16) |
             ((unsigned long)xbt[11] << 24);
    if (header < DN_XBT_FIXED_HEADER || header > DN_XBT_FIXED_HEADER + DN_XBT_MAX_PATH || header + 128 > size) {
        return 0;
    }
    return tex_scan_contains(g_shared_ids, (size_t)g_shared_id_count,
                             dn_top_identity(xbt, xbt + header, size - header, size - header));
}
