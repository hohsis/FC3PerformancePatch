/* Finds "stale" top mip level files: ones the game would add to a texture
 * that a mod has replaced. Pure C; file access goes through a callback.
 *
 * A texture mod is installed as the override archive (patch.fat / patch.dat),
 * whose files win over those of every other archive. When it replaces a base
 * texture but brings no top level file of its own, the game still streams in
 * the ORIGINAL top level of that texture. If that file has the wrong size the
 * texture guard refuses it (tex_rule.h). If it happens to have the right size
 * nothing crashes, but the original picture replaces the mod's within a few
 * metres of the camera - the ground visibly changes texture as you walk.
 *
 * The engine cannot tell the two apart and neither can a hook on its texture
 * class, which only sees the pixels. So the archives are read once at start:
 * for every base texture in the override archive that names a top level file
 * found only in the game's own archives, with a size that fits, the override's
 * base is compared with the game's copy of the same file. If they differ, the
 * top level belongs to a picture that is no longer there, and its identity
 * (dn_top_identity) is put on a list. The texture guard refuses listed files.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_TEX_SCAN_H
#define FC3PP_TEX_SCAN_H

#include <stddef.h>

#include "dunia.h"

/* Reads exactly len bytes at offset of archive number `archive`'s data file.
 * Returns 0 on success. */
typedef int (*ts_read_fn)(void *ctx, unsigned archive, unsigned long long offset, void *buf, size_t len);

typedef struct ts_input {
    const dn_fat *fats; /* fats[0] is the override archive, the rest the game's */
    unsigned count;
    ts_read_fn read;
    void *ctx;
} ts_input;

typedef struct ts_stats {
    unsigned textures;  /* texture files in the override archive */
    unsigned linked;    /* ... that name a top level file */
    unsigned own_top;   /* ... and bring that file themselves */
    unsigned missing;   /* top level file found nowhere */
    unsigned unfit;     /* the game's top level has another size or format: refused anyway */
    unsigned unchanged; /* the override's base is the game's own: the pair is intact */
    unsigned stale;     /* replaced base, original top level that fits */
    unsigned truncated; /* identities that did not fit into the list */
} ts_stats;

/* How much of two copies of a base texture is compared. */
#define TS_COMPARE_BYTES 16384u

/* Fills ids (sorted, without duplicates) and returns how many were stored,
 * or -1 when memory ran out. stats may be NULL. */
long tex_scan_stale(const ts_input *in, unsigned long long *ids, size_t cap, ts_stats *stats);

/* ---- top level files that several textures name --------------------------------
 * In the game's own data each top level file is named by one texture. With a
 * mod's archive on top that no longer holds, and the engine is not built for
 * it (tex_link.h). A top level file counts as shared when
 *   - two or more textures of the override archive name it, or
 *   - one texture of the override archive names it that is not its original
 *     owner (the game's copy of that texture, if there is one, names another
 *     file or none), and the file exists in the game's archives - where its
 *     original owner still names it too.
 * That needs only the override archive's textures and single look-ups in the
 * game's archives; on a real modded installation it finds exactly the files
 * a comparison of every texture of the game finds. */
typedef struct ts_shared_stats {
    unsigned among_override; /* named by two or more of the override archive's textures */
    unsigned with_game;      /* named by one of them and by a texture of the game */
    unsigned truncated;      /* names or identities that did not fit */
} ts_shared_stats;

/* Fills names with the name hashes of the shared top level files (sorted,
 * without duplicates) and ids with the identities (dn_top_identity) of every
 * copy of those files in the archives (sorted, without duplicates; *id_count
 * receives their number). Returns the number of names, or -1 when memory ran
 * out. ids and stats may be NULL. */
long tex_scan_shared(const ts_input *in, unsigned long long *names, size_t name_cap, unsigned long long *ids,
                     size_t id_cap, size_t *id_count, ts_shared_stats *stats);

/* Is id on a list produced by tex_scan_stale or tex_scan_shared? */
int tex_scan_contains(const unsigned long long *ids, size_t count, unsigned long long id);

#endif /* FC3PP_TEX_SCAN_H */
