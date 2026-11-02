/* config.c -- lightweight single-section INI configuration parser
 *
 * Reads/writes config.ini as:
 *   [config]
 *   key = value
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "config.h"

// CONFIG_COMMENT is a no-op for parsing/defaults -- it only does something in
// write_config (prints a "; ..." section header line), so config.ini comes
// out grouped and readable instead of one flat list of keys.
#define CONFIG_VARS \
  CONFIG_COMMENT("--- Language ---"); \
  CONFIG_VAR_STR(language); \
  CONFIG_COMMENT("--- Font ---"); \
  CONFIG_VAR_INT(game_font); \
  CONFIG_VAR_STR(game_font_path); \
  CONFIG_VAR_FLOAT(game_font_scale); \
  CONFIG_COMMENT("--- Text Shadow ---"); \
  CONFIG_VAR_INT(text_shadow); \
  CONFIG_VAR_FLOAT(text_shadow_scale); \
  CONFIG_VAR_INT(text_shadow_alpha); \
  CONFIG_COMMENT("--- Input / Controller ---"); \
  CONFIG_VAR_INT(right_stick_mirror); \
  CONFIG_VAR_STR(key_zl); \
  CONFIG_VAR_STR(key_zr); \
  CONFIG_VAR_STR(key_plus); \
  CONFIG_VAR_STR(key_minus); \
  CONFIG_COMMENT("--- Screen ---"); \
  CONFIG_VAR_INT(screen_width_handheld); \
  CONFIG_VAR_INT(screen_height_handheld); \
  CONFIG_VAR_INT(screen_width_docked); \
  CONFIG_VAR_INT(screen_height_docked); \
  CONFIG_VAR_FLOAT(field_zoom); \
  CONFIG_VAR_FLOAT(map_zoom); \
  CONFIG_VAR_INT(ui_scale_fix); \
  CONFIG_COMMENT("--- Graphics ---"); \
  CONFIG_VAR_INT(remove_bilinear_filter); \
  CONFIG_VAR_INT(force_nearest); \
  CONFIG_COMMENT("--- Performance ---"); \
  CONFIG_VAR_INT(fixed_timestep); \
  CONFIG_COMMENT("--- Performance (experimental) ---"); \
  CONFIG_VAR_INT(cache_palettes); \
  CONFIG_COMMENT("--- Enhancements ---"); \
  CONFIG_VAR_INT(remove_mobile_ui); \
  CONFIG_VAR_INT(menu_alignment_fix); \

// Dev-only knobs: recognized on read (so they can still be hand-tuned in
// config.ini) but deliberately left out of CONFIG_VARS above, so write_config
// never emits them and a freshly generated config.ini never surfaces them to
// end users. set_defaults() still seeds all of these, so they're fully
// functional at their default value even though config.ini never shows them.
// See map_zoom_y_trim's comment in config.h for why that one in particular
// stays hidden.
#define CONFIG_VARS_HIDDEN \
  CONFIG_VAR_FLOAT(game_font_xscale); \
  CONFIG_VAR_INT(gl_threaded); \
  CONFIG_VAR_INT(gl_no_error); \
  CONFIG_VAR_INT(cursor_fix); \
  CONFIG_VAR_INT(game_area_width_fix); \
  CONFIG_VAR_INT(field_zoom_fix); \
  CONFIG_VAR_INT(map_zoom_fix); \
  CONFIG_VAR_INT(native_controller); \
  CONFIG_VAR_INT(controller_glyphs); \
  CONFIG_VAR_INT(fix_diagonal_movement); \
  CONFIG_VAR_FLOAT(map_zoom_y_trim);

Config config;

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_FLOAT(var) if (!strcmp(name, #var)) { config.var = atof(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { strlcpy(config.var, value, sizeof(config.var)); return; }
  #define CONFIG_COMMENT(text) // no-op when parsing -- comments aren't real keys
  CONFIG_VARS
  CONFIG_VARS_HIDDEN
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR
  #undef CONFIG_COMMENT
}

// Trims leading/trailing whitespace (and a trailing \r, for files saved with
// CRLF line endings) in place. Returns the start of the trimmed string -- the
// original pointer may no longer point at it, so always use the return value.
static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) ++s;
  if (!*s) return s;
  char *end = s + strlen(s) - 1;
  while (end > s && (isspace((unsigned char)*end) || *end == '\r')) *end-- = 0;
  return s;
}

static void set_defaults(void) {
  memset(&config, 0, sizeof(Config));
  config.screen_width_handheld = 1280;
  config.screen_height_handheld = 720;
  config.screen_width_docked = 1920;
  config.screen_height_docked = 1080;
  strlcpy(config.language, LANG_DEFAULT, sizeof(config.language));
  config.gl_threaded = 0;   // off by default -- enable per-device if you want the extra headroom
  config.gl_no_error = 1;   // skip mesa's GL call validation by default
  config.game_font = 1;     // try the in-game font for system-font labels (testing)
  strlcpy(config.game_font_path, "/switch/ct/font/ChronoType/ChronoType.ttf", sizeof(config.game_font_path));
  config.game_font_scale = 0.90f; // 1.0 = auto-fit cap height to the shared font
  config.game_font_xscale = 1.0f; // 1.0 = no horizontal squeeze
  config.text_shadow = 1;         // crisp CT-style drop shadow on system-font labels
  config.text_shadow_alpha = 200; // shadow opacity 0..255 (black)
  config.text_shadow_scale = 1.0f; // offset multiplier (auto offset scales w/ size)
  config.cursor_fix = 1;           // keep selected text WHITE, dark highlight
  config.remove_bilinear_filter = 1; // force GL_NEAREST (pixel-perfect filtering)
  config.remove_mobile_ui = 1;     // hide touch-overlay buttons
  config.native_controller = 1;    // native controller input -> Switch-button prompts
  config.controller_glyphs = 1;    // force <BTN_*> dialogue tags to the pad glyph set
  config.fix_diagonal_movement = 1; // smooth diagonal movement (matches cardinal speed)
  config.cache_palettes = 0;        // share world-map palette textures instead of rebuilding them per frame (experimental)
  config.right_stick_mirror = 1;    // right stick emulates the left stick (movement)
  strlcpy(config.key_zl,    "key_zl",    sizeof(config.key_zl));    // unused buttons remap to themselves by default (no-op)
  strlcpy(config.key_zr,    "key_zr",    sizeof(config.key_zr));
  strlcpy(config.key_plus,  "key_plus",  sizeof(config.key_plus));
  strlcpy(config.key_minus, "key_minus", sizeof(config.key_minus));
  config.fixed_timestep = 1;        // constant 1/60 dt -> removes dt-jitter drift (anim/audio sync)
  config.ui_scale_fix = 1;            // stamp the whole design-resolution aspect table (640x360, both modes)
  config.force_nearest = 0;           // off by default; enforce NEAREST on every texture at the GL wrapper (kills the bilinear upscale proven by the framebuffer screenshot)
  config.game_area_width_fix = 1;     // adaptive ctr::gameArea width (UI-layer consumers); no-op at stock 568 design
  config.menu_alignment_fix = 1;      // Equipment menu: drop stat/HP-block/button/equip-info text 1.5u to match the item list rows
  config.field_zoom_fix = 1;          // square+integer field pixels (320x180 view); boot-time patch, relaunch to change
  config.field_zoom = 2.0f;           // 4x4 handheld / 6x6 art-px (original field_zoom_fix framing); lower = more map visible
  config.map_zoom_fix = 1;            // on by default
  config.map_zoom = 2.0f;             // 4x4 handheld / 6x6 art-px, matching field_zoom_fix's framing, once enabled
  config.map_zoom_y_trim = 12.0f;     // dev-only fine-trim; hidden from write_config, see config.h
  strlcpy(config.mods_dir, "mods", sizeof(config.mods_dir)); // .ctp mod packs folder
}

// One-time migration from the old flat "key value" config.txt, for anyone
// upgrading from before config.ini existed. Deliberately separate from the
// INI parser below -- it's the old format's own simpler whitespace-split
// rule, not '='-split, so it has to be read on its own terms. Returns 0 if a
// legacy file was found and at least opened (regardless of how many lines in
// it parsed), -1 if there was nothing to migrate.
static int read_legacy_txt(const char *file) {
  char line[1024];
  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  while (fgets(line, sizeof(line), f) != NULL) {
    char *name = trim(line);
    if (!*name || *name == '#')
      continue; // blank line or comment

    char *tmp = name;
    while (*tmp && !isspace((unsigned char)*tmp)) ++tmp;
    if (!*tmp) continue; // no value on this line
    *tmp = 0;
    char *value = trim(tmp + 1);
    parse_var(name, value);
  }

  fclose(f);
  return 0;
}

// Lightweight single-section INI reader: only lines inside "[config]" are
// honoured (anything in another section, or before any header at all, is
// ignored rather than guessed at). This project only ever has the one
// section, so it deliberately doesn't build a generic section/key map the
// way a multi-document INI library would -- known keys are dispatched
// straight into the Config struct via parse_var, same as the old format did.
int read_config(const char *file) {
  char line[1024];
  char section[32] = { 0 };

  set_defaults();

  FILE *f = fopen(file, "r");
  if (f == NULL) {
    // config.ini doesn't exist yet -- if there's an old config.txt, migrate
    // its values forward instead of silently resetting everything to
    // defaults, then retire it so it's never read again.
    if (read_legacy_txt(CONFIG_LEGACY_NAME) == 0) {
      rename(CONFIG_LEGACY_NAME, CONFIG_LEGACY_NAME ".bak");
      return 0;
    }
    return -1;
  }

  while (fgets(line, sizeof(line), f) != NULL) {
    char *l = trim(line);
    if (!*l || *l == ';' || *l == '#')
      continue; // blank line or comment

    const size_t len = strlen(l);
    if (l[0] == '[' && l[len - 1] == ']') {
      l[len - 1] = 0;
      strlcpy(section, l + 1, sizeof(section));
      continue;
    }

    if (strcmp(section, "config") != 0)
      continue; // not our section -- ignore (forward-compat with anything
                // else that might one day share this file)

    char *eq = strchr(l, '=');
    if (!eq) continue; // no '=' -- malformed line, skip rather than guess
    *eq = 0;
    char *key = trim(l);
    char *value = trim(eq + 1);
    if (*key) parse_var(key, value);
  }

  fclose(f);

  // language must never be empty (a malformed line could blank it); the rest of
  // the engine indexes localisation tables with it.
  if (!config.language[0])
    strlcpy(config.language, LANG_DEFAULT, sizeof(config.language));

  return 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  fprintf(f, "[config]\n");

  // Blank line before every section header except the first one (which
  // already sits right under "[config]"), so the file reads as visually
  // separated groups instead of one unbroken block.
  int first_comment = 1;

  #define CONFIG_VAR_INT(var) fprintf(f, "%s = %d\n", #var, config.var)
  #define CONFIG_VAR_FLOAT(var) fprintf(f, "%s = %g\n", #var, config.var)
  // write strings verbatim (empty stays empty); language is guaranteed non-empty
  // by read_config, so it never needs the old LANG_DEFAULT fallback here.
  #define CONFIG_VAR_STR(var) fprintf(f, "%s = %s\n", #var, config.var)
  #define CONFIG_COMMENT(text) do { \
    if (!first_comment) fprintf(f, "\n"); \
    first_comment = 0; \
    fprintf(f, "; %s\n", text); \
  } while (0)
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR
  #undef CONFIG_COMMENT

  fclose(f);

  return 0;
}