#ifndef __PALETTE_CACHE_H__
#define __PALETTE_CACHE_H__

#include "so_util.h"

// Share world-map palette textures instead of creating one per object per
// frame (see palette_cache.c). config.cache_palettes.
void apply_palette_cache(so_module *mod); // before so_finalize()
void palette_cache_frame(void);           // once per frame, before rendering

#endif
