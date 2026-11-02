/* palette_cache.c -- share per-frame palette textures (config: cache_palettes)
 *
 * WHY
 * Palette-shaded sprites get a brand-new palette Texture2D every frame: the
 * game builds the colours, creates a texture for them (glTexImage2D upload),
 * binds it as the shader's u_texture1 and lets the autorelease pool destroy it
 * at the end of the frame. libchrono 2.1.5 does this in:
 *   - WorldObject::setColor() (0x63f044): world-map objects (the Black Omen,
 *     Ozzie's fort, the Epoch, ...), 256x1 palette. Near a big object that's
 *     ~65 texture creations + destructions per frame: the profiler measured
 *     ~14 ms of Texture2D::initWithData plus ~2.4 ms of ~Texture2D per frame
 *     at stock clocks.
 *   - Character::update(int) (0x5552ec): every visible character, every frame
 *     (FieldImpl::charaSort on the field, BattleRender::__render_oam in
 *     battle), 16x1 palette.
 *   - Character::update(Color4B*) (0x5556ac): the same for an explicit
 *     palette (battle effects, Frog, resetPalette / setDefaultPalette).
 *   - EffectBg::update() (0x55f5b8) / EffectBg::updatePalette() (0x55f6d8):
 *     the battle BG3 effect layer, once per cell (BattleRender::render_bg3_cell),
 *     16x1 palette.
 *
 * WHAT THIS DOES
 * Each function's GOT slot points at a line-for-line re-implementation that
 * looks the palette up in a cache (keyed by the exact colour data the original
 * uses) and creates a texture only for palettes it hasn't seen. Everything
 * else is identical: same texture contents, format, size and filtering, same
 * uniform, same calls in the same order, through the same virtual slots.
 * Textures evicted from the cache are released at the start of the next frame,
 * i.e. after the frame that may still draw with them -- the same lifetime an
 * autoreleased texture had.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "palette_cache.h"

#define BUCKETS 1024 // direct-mapped; a miss just replaces the bucket's entry
#define KEY_MAX 64

// what a cache entry's texture is built from (part of the key):
// WorldObject's 15 colours -> 256x1, or 16 colours as they are -> 16x1
enum { PAL_WORLD, PAL_16 };

typedef struct { float w, h; } Size;
typedef struct { uint8_t key[KEY_MAX]; uint8_t kind, used; void *tex; } Entry;

static Entry s_cache[BUCKETS];
static void **s_pending;   // evicted textures, released next frame
static int s_npending, s_cappending;
static int s_active;

static void *(*cxx_new)(size_t);
static void  (*Texture2D_ctor)(void *);
static int   (*Texture2D_initWithData)(void *, const void *, long, int, int, int, const Size *);
static void  (*Texture2D_setAlias)(void *);
static void  (*Ref_retain)(void *);
static void  (*Ref_release)(void *);
static void  (*GLProgramState_setUniformTexture)(void *, const void *, void *);
static uint8_t *(*ChronoCanvas_getInstance)(void);

static uint32_t hash_key(int kind, const uint8_t *k, int len) {
  uint32_t h = 2166136261u ^ (uint32_t)kind; // FNV-1a
  for (int i = 0; i < len; i++) { h ^= k[i]; h *= 16777619u; }
  return h;
}

static void defer_release(void *tex) {
  if (s_npending == s_cappending) {
    const int n = s_cappending ? s_cappending * 2 : 64;
    void **p = realloc(s_pending, n * sizeof(void *));
    if (!p) { Ref_release(tex); return; } // out of memory: best effort
    s_pending = p; s_cappending = n;
  }
  s_pending[s_npending++] = tex;
}

// as the originals: new Texture2D, initWithData RGBA8888, setAliasTexParameters
// on success. Refcount 1, owned by the cache.
static void *make_texture(const void *data, int len, int w, int h) {
  void *tex = cxx_new(0x88); // sizeof(cocos2d::Texture2D) in this build
  Texture2D_ctor(tex);
  const Size sz = { (float)w, (float)h };
  if (Texture2D_initWithData(tex, data, len, 2 /* RGBA8888 */, w, h, &sz) & 0xff)
    Texture2D_setAlias(tex);
  return tex;
}

static void *make_world_texture(const uint8_t *key) {
  // 256 RGBA entries, zeroed; 8 blocks of 16 colours, each block =
  // transparent black followed by the 15 key colours
  uint8_t data[1024];
  memset(data, 0, sizeof(data));
  for (int b = 0; b < 8; b++)
    memcpy(data + b * 64 + 4, key, 60);
  return make_texture(data, sizeof(data), 256, 1);
}

static void *make_16_texture(const uint8_t *key) {
  return make_texture(key, 64, 16, 1); // the 16 colours as they are
}

static void *cached_texture(int kind, const uint8_t *key, int len) {
  Entry *e = &s_cache[hash_key(kind, key, len) & (BUCKETS - 1)];
  if (!e->used || e->kind != kind || memcmp(e->key, key, len) != 0) {
    if (e->used) defer_release(e->tex);
    memcpy(e->key, key, len);
    e->kind = (uint8_t)kind;
    e->tex = kind == PAL_WORLD ? make_world_texture(key) : make_16_texture(key);
    e->used = 1;
  }
  return e->tex;
}

// libc++ short std::string "u_texture1" (size<<1 in byte 0), as the originals
static const uint8_t U_TEXTURE1[24] = { 10 << 1, 'u', '_', 't', 'e', 'x', 't', 'u', 'r', 'e', '1', 0 };

// WorldObject: +0x0 cocos2d::Sprite*, +0x8 cocos2d::GLProgramState*
static void palette_set_color(void *self, const uint8_t *pal) {
  // the original copies colours 1..15 (bytes 4..63)
  void *tex = cached_texture(PAL_WORLD, pal + 4, 60);

  void *sprite = ((void **)self)[0];
  void *state  = ((void **)self)[1];
  GLProgramState_setUniformTexture(state, U_TEXTURE1, tex);

  void **vt = *(void ***)sprite;
  ((void (*)(void *, void *))vt[0x310 / 8])(sprite, state);          // setGLProgramState
  ((void (*)(void *, float, float))vt[0x90 / 8])(sprite, 2.f, 2.f); // setScale(2, 2)
}

// Character:
//   +0x0cf bool     palette updates disabled
//   +0x0db bool     drawn with the palette shader (set by Character::load)
//   +0x0e0 std::vector  non-empty: colours equal to the default ones go transparent
//   +0x110 std::vector<Color4B>  default palette
//   +0x128 cocos2d::GLProgramState*
// ChronoCanvas + 0xed5e + idx*64: palette idx (16 Color4B, unaligned)
static int chara_palette_on(const uint8_t *self) {
  return !(self[0xcf] & 1) && self[0xdb];
}

static void chara_set_texture(uint8_t *self, void *tex) {
  GLProgramState_setUniformTexture(*(void **)(self + 0x128), U_TEXTURE1, tex);
}

static void palette_chara_update_idx(uint8_t *self, int idx) {
  if (!chara_palette_on(self)) return;
  const uint8_t *src = ChronoCanvas_getInstance() + 0xed5e + (intptr_t)(idx * 16) * 4;
  const uint8_t *def = *(uint8_t **)(self + 0x110);
  const size_t ndef = (size_t)(*(uint8_t **)(self + 0x118) - def) / 4;
  const int clear_default = *(void **)(self + 0xe0) != *(void **)(self + 0xe8);

  // colour 0 and every colour past the default palette's size stay (0,0,0,0)
  uint8_t pal[64];
  memset(pal, 0, sizeof(pal));
  for (size_t i = 1; i < 16 && i < ndef; i++) {
    if (clear_default && memcmp(def + i * 4, src + i * 4, 4) == 0) continue;
    memcpy(pal + i * 4, src + i * 4, 4);
  }
  chara_set_texture(self, cached_texture(PAL_16, pal, 64));
}

static void palette_chara_update_pal(uint8_t *self, uint8_t *pal) {
  if (!chara_palette_on(self)) return;
  pal[3] = 0; // the original makes colour 0 transparent in the caller's array
  chara_set_texture(self, cached_texture(PAL_16, pal, 64));
}

// EffectBg (battle BG3 effect layer):
//   +0xc0 cocos2d::Color4B*  current palette (16 colours)
//   +0xd8 cocos2d::GLProgramState*
static void effect_bg_set_texture(uint8_t *self, const uint8_t *pal) {
  GLProgramState_setUniformTexture(*(void **)(self + 0xd8), U_TEXTURE1,
                                   cached_texture(PAL_16, pal, 64));
}

static void palette_effect_bg_update(uint8_t *self) {
  effect_bg_set_texture(self, *(uint8_t **)(self + 0xc0));
}

// colour 0 transparent, colours [from, to) from pal, the rest from the
// current palette (the original doesn't bound from / to; clamped here)
static void palette_effect_bg_update_palette(uint8_t *self, const uint8_t *pal, int from, int to) {
  const uint8_t *cur = *(uint8_t **)(self + 0xc0);
  const int lo = from < 0 ? 0 : from > 16 ? 16 : from;
  const int hi = to < 0 ? 0 : to > 16 ? 16 : to;
  uint8_t out[64];
  memset(out, 0, sizeof(out));
  if (lo > 1) memcpy(out + 4, cur + 4, (size_t)(lo - 1) * 4);
  if (lo < hi) memcpy(out + lo * 4, pal + lo * 4, (size_t)(hi - lo) * 4);
  if (hi < 16) memcpy(out + hi * 4, cur + hi * 4, (size_t)(16 - hi) * 4);
  effect_bg_set_texture(self, out);
}

void palette_cache_frame(void) {
  if (!s_active) return;
  for (int i = 0; i < s_npending; i++) Ref_release(s_pending[i]);
  s_npending = 0;
}

static uintptr_t *got_slot(so_module *mod, const char *name) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.plt") != 0) continue;
    Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    const size_t cnt = mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela);
    for (size_t j = 0; j < cnt; j++) {
      if (ELF64_R_TYPE(rels[j].r_info) != R_AARCH64_JUMP_SLOT) continue;
      const Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
      if (strcmp(mod->dynstrtab + sym->st_name, name) == 0)
        return (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
    }
  }
  return NULL;
}

// Point sym's GOT slot at hook if sym is the libchrono 2.1.5 function the hook
// re-implements: at offset off, with these instruction words at these offsets.
typedef struct { uint32_t at, word; } Check;

static int hook(so_module *mod, const char *sym, uintptr_t off,
                const Check *checks, int nchecks, void *fn) {
  const uintptr_t addr = so_try_find_addr_rx(mod, sym);
  const uint32_t *code = (const uint32_t *)((uintptr_t)mod->load_base + off);
  int ok = addr && addr - (uintptr_t)mod->load_virtbase == off;
  for (int i = 0; ok && i < nchecks; i++)
    ok = code[(checks[i].at - off) / 4] == checks[i].word;
  uintptr_t *slot = ok ? got_slot(mod, sym) : NULL;
  if (!slot) {
    debugPrintf("palette_cache: unexpected %s, not hooked\n", sym);
    return 0;
  }
  *slot = (uintptr_t)fn;
  return 1;
}

#define RESOLVE(var, sym) \
  do { *(uintptr_t *)&(var) = so_try_find_addr_rx(mod, sym); \
       if (!(var)) { debugPrintf("palette_cache: missing %s, not applied\n", sym); return; } } while (0)

void apply_palette_cache(so_module *mod) {
  if (!config.cache_palettes) return;

  RESOLVE(Texture2D_ctor, "_ZN7cocos2d9Texture2DC1Ev");
  RESOLVE(Texture2D_initWithData, "_ZN7cocos2d9Texture2D12initWithDataEPKvlNS0_11PixelFormatEiiRKNS_4SizeE");
  RESOLVE(Texture2D_setAlias, "_ZN7cocos2d9Texture2D21setAliasTexParametersEv");
  RESOLVE(Ref_retain, "_ZN7cocos2d3Ref6retainEv");
  RESOLVE(Ref_release, "_ZN7cocos2d3Ref7releaseEv");
  RESOLVE(GLProgramState_setUniformTexture,
          "_ZN7cocos2d14GLProgramState17setUniformTextureERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEPNS_9Texture2DE");
  RESOLVE(ChronoCanvas_getInstance, "_ZN12ChronoCanvas11getInstanceEv");
  uintptr_t *new_slot = got_slot(mod, "_Znwm"); // operator new, imported from libc++_shared
  if (!new_slot || !*new_slot) { debugPrintf("palette_cache: operator new not resolved, not applied\n"); return; }
  cxx_new = (void *(*)(size_t))*new_slot;

  // first instruction, bl Texture2D::initWithData, bl setUniformTexture
  static const Check set_color[] = {
    { 0x63f044, 0xa9bb7bfdu }, { 0x63f2d4, 0x9413ff63u }, { 0x63f31c, 0x9413ff59u } };
  static const Check update_idx[] = {
    { 0x5552ec, 0xd102c3ffu }, { 0x5554a0, 0x9417a6f0u }, { 0x5554e8, 0x9417a6e6u } };
  static const Check update_pal[] = {
    { 0x5556ac, 0xd10143ffu }, { 0x555724, 0x9417a64fu }, { 0x55576c, 0x9417a645u } };
  static const Check bg_update[] = {
    { 0x55f5b8, 0xd10143ffu }, { 0x55f61c, 0x94177e91u }, { 0x55f664, 0x94177e87u } };
  static const Check bg_update_palette[] = {
    { 0x55f6d8, 0xd10283ffu }, { 0x55f8e0, 0x94177de0u }, { 0x55f928, 0x94177dd6u } };

  int n = 0;
  n += hook(mod, "_ZN11WorldObject8setColorEPN7cocos2d7Color4BE", 0x63f044,
            set_color, 3, palette_set_color);
  n += hook(mod, "_ZN9Character6updateEi", 0x5552ec,
            update_idx, 3, palette_chara_update_idx);
  n += hook(mod, "_ZN9Character6updateEPN7cocos2d7Color4BE", 0x5556ac,
            update_pal, 3, palette_chara_update_pal);
  n += hook(mod, "_ZN8EffectBg6updateEv", 0x55f5b8,
            bg_update, 3, palette_effect_bg_update);
  n += hook(mod, "_ZN8EffectBg13updatePaletteEPN7cocos2d7Color4BEii", 0x55f6d8,
            bg_update_palette, 3, palette_effect_bg_update_palette);
  s_active = n > 0;
  debugPrintf("palette_cache: applied (%d of 5 hooks)\n", n);
}
