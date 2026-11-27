/* Draw-distance presets. Pure C.
 *
 * The engine's detail and distance settings are per-quality sections of its
 * render configuration, an XML document:
 *
 *     <Profile ...>
 *       <Geometry> <quality id="ultrahigh" LodScale="0.5" KillLodScale="0.5" .../> ... </Geometry>
 *       <Shadow>   <quality id="ultrahigh" SunShadowRange0="3" .../> ...           </Shadow>
 *       <Terrain>  <quality id="high" TerrainLodScale="1.0" .../> ...              </Terrain>
 *     </Profile>
 *
 * The defaults come from engine\settings\DefaultRenderConfig.xml in the game's
 * archives. The player's GamerProfile.xml has a <RenderProfile> element of the
 * same class, and sections placed in it override the defaults - the long-known
 * manual tweak. The game drops such sections again whenever it rewrites the
 * file, which is why the tweak never sticks.
 *
 * A preset takes the installed defaults (so it scales whatever a mod has put
 * there), multiplies the distance values (up for the "far" presets, down for
 * the "near" ones), and adds the resulting sections to the profile text the
 * game is about to parse.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_GFX_PRESET_H
#define FC3PP_GFX_PRESET_H

#include <stddef.h>

/* The numbers of the first three are what the INI file has always held. The
 * two "near" levels do the opposite of the "far" ones, for devices on which
 * the game's own processor work is the limit: less is drawn. */
typedef enum gp_level { GP_ORIGINAL = 0, GP_FAR = 1, GP_VERY_FAR = 2, GP_NEAR = 3, GP_VERY_NEAR = 4 } gp_level;

#define GP_LEVELS 5

/* The levels in the order a person thinks of them: -2 very near, -1 near,
 * 0 original, 1 far, 2 very far. This is also what the INI file holds. */
int gp_level_to_step(gp_level level);
/* Returns 0 and leaves *level alone when step is not one of the five. */
int gp_level_from_step(int step, gp_level *level);
const char *gp_level_name(gp_level level);
#define GP_OVERLAY_SLACK 64u

/* Text of profile with the preset's sections added to its <RenderProfile>.
 * render_config is the default render configuration. A section the profile
 * already has (the player's own tweak) is left alone. Returns the length
 * written to out (not NUL-terminated beyond it), or -1 when nothing was added:
 * level is GP_ORIGINAL, a text is not understood, or out is too small: it must
 * hold profile_len + config_len + GP_OVERLAY_SLACK bytes.
 * *sections (optional) receives the number of sections added. */
long gp_overlay(const char *profile, size_t profile_len, const char *render_config, size_t config_len, gp_level level,
                char *out, size_t cap, unsigned *sections);

/* One section of the render configuration with the preset applied, for
 * example "Geometry". Returns its length, or -1. *values (optional) receives
 * the number of attribute values changed. */
long gp_scaled_section(const char *render_config, size_t config_len, const char *section, gp_level level, char *out,
                       size_t cap, unsigned *values);

#endif /* FC3PP_GFX_PRESET_H */
