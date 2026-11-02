/* main.c -- Chrono Trigger (cocos2d-x 3.14.1) Switch wrapper entry point
 *
 * Copyright (C) 2026 NaGaa95 <https://github.com/NaGaa95>
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Loads libc++_shared.so + libchrono.so into emulated-Android memory, wires the
 * engine's imports to native shims, then drives the cocos2d-x JNI lifecycle
 * (JNI_OnLoad -> setContext/apk/assets -> nativeInit -> nativeRender loop).
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <EGL/egl.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "asset.h"
#include "gfx.h"
#include "opensles.h"
#include "prefs.h"
#include "movie_player.h"
#include "cache_progress.h"
#include "patches.h"
#include "palette_cache.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module cpp_mod;   // libc++_shared.so
so_module game_mod;  // libchrono.so

void ct_resolve_imports(so_module *mod);

// provide a replacement heap init so the newlib heap is separate from the .so
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // The newlib heap backs BOTH the engine's malloc and mesa/nouveau's GPU
  // texture memory (nvMap buffers come from this heap). cocos2d-x + Chrono
  // Trigger's field maps allocate hundreds of MB of textures/render targets, so
  // the newlib heap must get the bulk of memory -- only a small fixed slice is
  // reserved for the two .so load images (libc++_shared ~2MB + libchrono ~16MB).
  size_t so_reserve = (size_t)SO_HEAP_RESERVE_MB * 1024 * 1024;
  if (so_reserve > size / 2)
    so_reserve = size / 2;
  fake_heap_size = size - so_reserve;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)    fatal_error("Could not find\n%s.\nCheck your data files.", SO_NAME);
  if (stat(SOCPP_NAME, &st) < 0) fatal_error("Could not find\n%s.\nCheck your data files.", SOCPP_NAME);
  if (stat(ASSETS_DIR, &st) < 0) fatal_error("Could not find the\n%s/ folder.\nCheck your data files.", ASSETS_DIR);
}

static void set_screen_size(AppletOperationMode mode) {
  // Per-mode resolution -- see the Config struct doc comment in config.h.
  //
  // The EGL surface we ask for here becomes the buffer HOS composites onto
  // the real physical panel. If those two sizes don't match exactly, the
  // system compositor has to rescale our frame every frame -- a stage that
  // lives entirely outside the engine and outside every patch in patches.h.
  // scale_diag.log caught this directly: screen_width_docked=2560 was being
  // squeezed down to a real 1920x1080 panel (a 0.75x resample), which is
  // exactly the "still shimmers, gets worse with a bigger buffer" symptom.
  //
  // So: query the real panel size from the OS first. A config override is
  // only honoured if it's an EXACT whole-number multiple of the panel in
  // both axes (deliberate integer supersampling -- harmless, still a clean
  // pixel grid). Anything else -- including the old fixed 1920x1080/1280x720
  // guesses -- falls back to the panel's native size, which guarantees a
  // true 1:1 buffer with zero compositor rescale.
  s32 panel_w = 0, panel_h = 0;
  appletGetDefaultDisplayResolution(&panel_w, &panel_h);
  if (panel_w <= 0 || panel_h <= 0) {
    // Query failed (shouldn't happen, but don't leave screen_width/height
    // at 0) -- fall back to the old hardcoded per-mode guesses.
    panel_w = (mode == AppletOperationMode_Console) ? 1920 : 1280;
    panel_h = (mode == AppletOperationMode_Console) ? 1080 : 720;
  }

  int cfg_w = (mode == AppletOperationMode_Console) ? config.screen_width_docked
                                                     : config.screen_width_handheld;
  int cfg_h = (mode == AppletOperationMode_Console) ? config.screen_height_docked
                                                     : config.screen_height_handheld;

  int w = panel_w, h = panel_h;
  if (cfg_w >= 320 && cfg_h >= 180 && cfg_w <= 3840 && cfg_h <= 2160) {
    // Honour ANY explicit value in range, whether or not it's an integer
    // multiple of the real panel. Non-multiple values make the system
    // compositor rescale our frame every frame (a soft/shimmery resample
    // instead of a clean pixel-doubled one, per scale_diag.log) -- accepted
    // here as the tradeoff for supporting arbitrary per-mode resolutions.
    w = cfg_w; h = cfg_h;
  } else if (cfg_w > 0 || cfg_h > 0) {
    debugPrintf("set_screen_size: config %dx%d out of the 320x180-3840x2160 "
                "range -- using native panel size %dx%d instead.\n",
                cfg_w, cfg_h, panel_w, panel_h);
  }
  screen_width = w; screen_height = h;
}

// ---------------------------------------------------------------------------
// EGL / GLES2 context on the default NWindow
// ---------------------------------------------------------------------------

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;
static EGLConfig  s_egl_config; // picked once in egl_init, reused by egl_resize

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  // cocos default GLContextAttrs: RGBA8888, depth24, stencil8, no MSAA.
  // Try a lighter 16-bit depth buffer first -- the 2D renderer's vertexZ/
  // clipping usage needs nowhere near 24-bit precision, and trimming it saves
  // a little framebuffer bandwidth. Fall back to the original 24/8 if this
  // GPU/driver only exposes a combined 24-bit-depth+8-bit-stencil format
  // (common on Tegra/nouveau), so a missing 16/8 combo never blocks boot.
  const EGLint cfg_attr_16[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  const EGLint cfg_attr_24[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr_16, &s_egl_config, 1, &num) || num < 1) {
    debugPrintf("egl: 16-bit depth config unavailable, falling back to 24-bit\n");
    if (!eglChooseConfig(s_display, cfg_attr_24, &s_egl_config, 1, &num) || num < 1) {
      debugPrintf("egl: no config\n");
      return 0;
    }
  }

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  // Explicitly pin the crop to the full new buffer. Crop is separate NWindow
  // state from the dimensions: across dock/undock surface recreation a crop
  // sized for the PREVIOUS buffer can persist, making the compositor present
  // a mismatched sub-region rescaled to the display -- sharp but non-uniform
  // pixels, identical in both modes, invisible to every in-game measurement.
  nwindowSetCrop(win, 0, 0, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, s_egl_config, win, NULL);
  if (!s_surface) { debugPrintf("egl: no surface\n"); return 0; }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_context = eglCreateContext(s_display, s_egl_config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1); // present every vsync: always target the panel's max (60)
  return 1;
}

// Re-target the render surface at the current screen_width/screen_height
// (call set_screen_size() first). Used for live dock/undock resolution
// switches. We deliberately destroy + recreate the EGLSurface rather than
// just calling nwindowSetDimensions on the live one and hoping the driver
// picks it up: that "transparent resize" behaviour is part of the EGL spec
// for normal desktop window systems, but this is the NVN/nvnflinger-backed
// nouveau winsys, not a standard windowing system, and whether an in-place
// resize is honoured there isn't something we can confirm without hardware
// testing both ways. Destroy-and-recreate is unambiguous: the new surface is
// guaranteed to be allocated at the new size by construction. The EGLContext
// is untouched -- a context is independent of any particular surface, so all
// GL object state (textures, buffers, shaders) survives the swap intact.
// last surface size that actually worked, for the egl_resize fallback below
static int s_egl_good_w = 0, s_egl_good_h = 0;

static int egl_resize(void) {
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_surface) { eglDestroySurface(s_display, s_surface); s_surface = EGL_NO_SURFACE; }

  NWindow *win = nwindowGetDefault();

  // Surface creation can fail transiently right after destroying the old
  // surface (the compositor may still hold the old buffers). The previous
  // version gave up on the first failure and returned with NO surface bound
  // -- the game kept running but nothing was ever presented again: a
  // permanent black screen. Now: retry the requested size a few times, then
  // fall back to the last size that worked rather than end up surfaceless.
  for (int attempt = 0; attempt < 5; attempt++) {
    int w = screen_width, h = screen_height;
    if (attempt >= 3 && s_egl_good_w > 0 &&
        (s_egl_good_w != screen_width || s_egl_good_h != screen_height)) {
      w = s_egl_good_w;  // final attempts: last-known-good fallback
      h = s_egl_good_h;
    }
    nwindowSetDimensions(win, w, h);
    // Explicitly pin the crop to the full new buffer. Crop is separate
    // NWindow state from the dimensions: across dock/undock surface
    // recreation a crop sized for the PREVIOUS buffer can persist, making
    // the compositor present a mismatched sub-region rescaled to the
    // display -- sharp but non-uniform pixels, identical in both modes,
    // invisible to every in-game measurement.
    nwindowSetCrop(win, 0, 0, w, h);
    s_surface = eglCreateWindowSurface(s_display, s_egl_config, win, NULL);
    if (s_surface) {
      if (w != screen_width || h != screen_height) {
        debugPrintf("egl: resize fell back to last-known-good %dx%d\n", w, h);
        screen_width = w;   // keep the engine's idea of the size truthful
        screen_height = h;
      }
      eglMakeCurrent(s_display, s_surface, s_surface, s_context);
      eglSwapInterval(s_display, 1);
      s_egl_good_w = w;
      s_egl_good_h = h;
      return 1;
    }
    debugPrintf("egl: resize attempt %d failed (%dx%d), retrying\n", attempt, w, h);
    svcSleepThread(20ull * 1000 * 1000); // 20 ms for in-flight buffers to drain
  }
  debugPrintf("egl: resize - no surface after retries\n");
  return 0;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// cocos2d-x engine entry points (exported by libchrono.so)
// ---------------------------------------------------------------------------

static int  (*e_JNI_OnLoad)(void *vm, void *reserved);
static void (*e_nativeSetContext)(void *env, void *thiz, void *ctx, void *amgr);
static void (*e_nativeSetApkPath)(void *env, void *thiz, void *path);
static void (*e_setAssetManager)(void *env, void *thiz, void *ctx, void *amgr);
static void (*e_setExternalStorageInfo)(void *env, void *thiz, void *a, void *b, void *c);
static void (*e_nativeInit)(void *env, void *thiz, int w, int h);
static void (*e_nativeOnSurfaceChanged)(void *env, void *thiz, int w, int h);
static void (*e_nativeRender)(void *env);  // NOTE: env only, no thiz
static void (*e_nativeOnPause)(void);
static void (*e_nativeOnResume)(void);
static void (*e_touchBegin)(void *env, void *thiz, int id, float x, float y);
static void (*e_touchEnd)(void *env, void *thiz, int id, float x, float y);
static void (*e_touchMove)(void *env, void *thiz, void *ids, void *xs, void *ys);
static int  (*e_keyEvent)(void *env, void *thiz, int keycode, int pressed);
// cocos GameControllerAdapter natives: deviceName (jstring) is the FIRST data arg
static void (*e_ctrlConnected)(void *env, void *thiz, void *name, int id);
static void (*e_ctrlButton)(void *env, void *thiz, void *name, int id, int button, int pressed, float value, int analog);
static void (*e_ctrlAxis)(void *env, void *thiz, void *name, int id, int axis, float value, int analog);
static void (*e_bitmapDC)(void *env, void *thiz, int w, int h, void *pixels);
static void (*e_videoCb)(void *env, void *thiz, int index, int event);
static void (*e_insertText)(void *env, void *thiz, void *jstr);
static void (*e_deleteBackward)(void *env, void *thiz);
// cocos ui::EditBox result natives (Cocos2dxEditBoxHelper)
static void (*e_ebDidBegin)(void *env, void *cls, int index);
static void (*e_ebChanged)(void *env, void *cls, int index, void *jstr);
static void (*e_ebDidEnd)(void *env, void *cls, int index, void *jstr);
static void (*e_glInvalidate)(void); // cocos2d::GL::invalidateStateCache (post-movie)
static void *(*e_director_getInstance)(void); // cocos2d::Director::getInstance() -- diagnostic only

// DeviceInfo::mCurrentLanguage (int): the engine indexes mLocalizationLanguages
// with this directly (ja=0 en=1 de=2 it=3 es=4 fr=5 zh-Hans=6 zh-Hant=7 ko=8).
// We pin it to config.language (see force_language + the patches in main()).
static int *e_lang_var;
static void *g_ctrl_name; // persistent jstring device name for controller events

#define RX(sym) so_try_find_addr_rx(&game_mod, sym)

static void resolve_entry_points(void) {
  e_JNI_OnLoad            = (void *)RX("JNI_OnLoad");
  e_nativeSetContext      = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetContext");
  e_nativeSetApkPath      = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
  e_setAssetManager       = (void *)RX("Java_org_cocos2dx_cpp_AppActivity_setAssetManager");
  e_setExternalStorageInfo= (void *)RX("Java_org_cocos2dx_cpp_AppActivity_setExternalStorageInfo");
  e_nativeInit            = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  e_nativeOnSurfaceChanged= (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnSurfaceChanged");
  e_nativeRender          = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  e_nativeOnPause         = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  e_nativeOnResume        = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  e_touchBegin            = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  e_touchEnd              = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  e_touchMove             = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
  e_keyEvent              = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyEvent");
  e_ctrlConnected         = (void *)RX("Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerConnected");
  e_ctrlButton            = (void *)RX("Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerButtonEvent");
  e_ctrlAxis              = (void *)RX("Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerAxisEvent");
  e_bitmapDC              = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC");
  e_videoCb               = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxVideoHelper_nativeExecuteVideoCallback");
  e_insertText            = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText");
  e_deleteBackward        = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward");
  e_ebDidBegin            = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxEditBoxHelper_editBoxEditingDidBegin");
  e_ebChanged             = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxEditBoxHelper_editBoxEditingChanged");
  e_ebDidEnd              = (void *)RX("Java_org_cocos2dx_lib_Cocos2dxEditBoxHelper_editBoxEditingDidEnd");
  e_glInvalidate          = (void *)RX("_ZN7cocos2d2GL20invalidateStateCacheEv");
  e_director_getInstance  = (void *)RX("_ZN7cocos2d8Director11getInstanceEv"); // diagnostic only
  e_lang_var              = (int *)RX("_ZN10DeviceInfo16mCurrentLanguageE");
}

static void *thiz; // fake MainActivity instance handed to the JNI entry points

// ---------------------------------------------------------------------------
// input -- Switch HID -> cocos keyboard + controller events
// Android keycodes (keyboard path) and cocos2d::Controller::Key (controller path)
// ---------------------------------------------------------------------------

// Android KeyEvent keycodes recognised by cocos2d-x 3.14.1's g_keyCodeMap
#define AK_BACK   4
#define AK_DUP    19
#define AK_DDOWN  20
#define AK_DLEFT  21
#define AK_DRIGHT 22
#define AK_DCENTER 23
#define AK_ENTER  66
#define AK_MENU   82
// Letter keys the engine binds to its non-confirm/cancel actions on the
// keyboard path (used only in keyboard-compat mode). The engine's own glyph
// tables reveal these bindings: MENU=V, WARP=B, L=G, R=H.
#define AK_V      50   // KEYCODE_V -> MENU action
#define AK_B      30   // KEYCODE_B -> WARP action
#define AK_G      35   // KEYCODE_G -> L action
#define AK_H      36   // KEYCODE_H -> R action
#define AK_NONE   (-1)

// cocos2d::Controller::Key enum values (CCController.h, 3.14.1)
enum {
  CC_JOY_LX = 1000, CC_JOY_LY, CC_JOY_RX, CC_JOY_RY,
  CC_BTN_A, CC_BTN_B, CC_BTN_C, CC_BTN_X, CC_BTN_Y, CC_BTN_Z,
  CC_DPAD_UP, CC_DPAD_DOWN, CC_DPAD_LEFT, CC_DPAD_RIGHT, CC_DPAD_CENTER,
  CC_L_SHOULDER, CC_R_SHOULDER, CC_L_TRIGGER, CC_R_TRIGGER,
  CC_L_THUMB, CC_R_THUMB, CC_BTN_START, CC_BTN_SELECT, CC_BTN_PAUSE
};

typedef struct {
  u64 mask;       // HidNpadButton bit (incl. stick-as-dpad synthesised below)
  int android_kc; // keyboard path keycode, or AK_NONE
  int cc_key;     // controller path cocos key
} KeyMap;

// Native controller layout. The engine (GameController / nsInput) binds its
// actions to cocos2d Controller::Key codes, and renders the matching on-screen
// glyph per button, so we map each Switch button 1:1 to the cocos key the
// engine expects -- this is what keeps the prompts showing real Switch buttons:
//   A = confirm        B = cancel / dash
//   Y = menu           X = warp / map        (the engine's own pad scheme,
//   L/R = page-shift                          which is why MENU=BtnY, WARP=BtnX)
// A/B/L/R match the DS/SNES layout exactly; only menu/warp sit on Y/X per the
// engine's controller binding (so the glyph always matches the button pressed).
// android_kc is emitted ONLY in keyboard-compat mode (native_controller == 0).
static const KeyMap g_keymap[] = {
  { HidNpadButton_A,     AK_ENTER,   CC_BTN_A },       // confirm
  { HidNpadButton_B,     AK_BACK,    CC_BTN_B },       // cancel / dash
  { HidNpadButton_X,     AK_B,       CC_BTN_X },       // warp / map
  { HidNpadButton_Y,     AK_V,       CC_BTN_Y },       // menu
  { HidNpadButton_L,     AK_G,       CC_L_SHOULDER },  // page / shift left
  { HidNpadButton_R,     AK_H,       CC_R_SHOULDER },  // page / shift right
  { HidNpadButton_ZL,    AK_NONE,    CC_L_TRIGGER },
  { HidNpadButton_ZR,    AK_NONE,    CC_R_TRIGGER },
  { HidNpadButton_Plus,  AK_MENU,    CC_BTN_START },   // pause / system menu
  { HidNpadButton_Minus, AK_NONE,    CC_BTN_SELECT },
  { HidNpadButton_StickL, AK_NONE,   CC_L_THUMB },
  { HidNpadButton_StickR, AK_NONE,   CC_R_THUMB },
  { HidNpadButton_Up    | HidNpadButton_StickLUp,    AK_DUP,    CC_DPAD_UP },
  { HidNpadButton_Down  | HidNpadButton_StickLDown,  AK_DDOWN,  CC_DPAD_DOWN },
  { HidNpadButton_Left  | HidNpadButton_StickLLeft,  AK_DLEFT,  CC_DPAD_LEFT },
  { HidNpadButton_Right | HidNpadButton_StickLRight, AK_DRIGHT, CC_DPAD_RIGHT },
};
#define NUM_KEYMAP (sizeof(g_keymap) / sizeof(*g_keymap))

static PadState pad;
static int g_prev[NUM_KEYMAP];

// ---- optional button remapping (config: key_zl / key_zr / key_plus / key_minus)
// Each of the four normally-unused buttons can be redirected to emit another
// button's events instead of its own. g_remap[i] is the keymap index whose
// cc_key/android_kc send_button() should emit when physical button i fires;
// it defaults to i (self = unchanged) for every entry, and only the four
// source rows are ever pointed elsewhere. Edge tracking in update_keys()
// stays keyed to the physical button (g_prev[]), so only the emitted event
// changes -- press/release of the real button still drives everything.
static int g_remap[NUM_KEYMAP];

// The remap targets we accept, by config token -> the HidNpadButton mask of
// the keymap row that implements them. Only face/shoulder buttons the engine
// actually binds are offered (a/b/x/y/l/r); this is also the set the user's
// config strings are validated against.
typedef struct { const char *token; u64 mask; } RemapTarget;
static const RemapTarget g_remap_targets[] = {
  { "key_a", HidNpadButton_A },
  { "key_b", HidNpadButton_B },
  { "key_x", HidNpadButton_X },
  { "key_y", HidNpadButton_Y },
  { "key_l", HidNpadButton_L },
  { "key_r", HidNpadButton_R },
};
#define NUM_REMAP_TARGETS (sizeof(g_remap_targets) / sizeof(*g_remap_targets))

// Find the keymap index whose (sole) mask bit is exactly `mask`. Returns -1 if
// no row matches -- used both to locate a remap source and its target row.
static int keymap_index_for_mask(u64 mask) {
  for (unsigned i = 0; i < NUM_KEYMAP; i++)
    if (g_keymap[i].mask == mask) return (int)i;
  return -1;
}

// Resolve one source button's config string to a target keymap index. `value`
// is the config token (e.g. "key_a"); `self_mask` is the source's own button.
// Unrecognised tokens, an empty string, or the source's own name all resolve
// to the source itself (no remap) -- exactly the "behave as is" fallback.
static void resolve_one_remap(u64 self_mask, const char *value) {
  const int src = keymap_index_for_mask(self_mask);
  if (src < 0) return; // source not in keymap (shouldn't happen); nothing to do
  for (unsigned t = 0; t < NUM_REMAP_TARGETS; t++) {
    if (strcmp(value, g_remap_targets[t].token) == 0) {
      const int dst = keymap_index_for_mask(g_remap_targets[t].mask);
      if (dst >= 0) g_remap[src] = dst; // valid target: redirect
      return;                           // (dst<0 can't happen for our set)
    }
  }
  // no match (self-name, blank, or anything unknown) -> leave g_remap[src]=src
}

// Build g_remap[] from config. Call once, after read_config(), before the
// input loop. Identity by default; only the four configured sources move.
static void init_button_remap(void) {
  for (unsigned i = 0; i < NUM_KEYMAP; i++) g_remap[i] = (int)i;
  resolve_one_remap(HidNpadButton_ZL,    config.key_zl);
  resolve_one_remap(HidNpadButton_ZR,    config.key_zr);
  resolve_one_remap(HidNpadButton_Plus,  config.key_plus);
  resolve_one_remap(HidNpadButton_Minus, config.key_minus);
}

static void send_button(int idx, int pressed) {
  // Emit the remap target's events (defaults to idx itself, i.e. unchanged).
  // Only the four configured source buttons (ZL/ZR/+/-) ever point elsewhere;
  // for every other button g_remap[idx] == idx, so this is a no-op lookup.
  const KeyMap *k = &g_keymap[g_remap[idx]];
  // Keyboard path: only when native_controller is OFF. The engine flips to
  // keyboard-style glyphs the moment it sees a key event, so suppressing these
  // is what keeps it in native controller mode (real Switch-button prompts).
  // Name entry is unaffected -- it goes through the IME path, not these events.
  if (!config.native_controller && k->android_kc != AK_NONE && e_keyEvent)
    e_keyEvent(fake_env, thiz, k->android_kc, pressed);
  if (e_ctrlButton && k->cc_key) // cc_key 0 = no controller mapping
    e_ctrlButton(fake_env, thiz, g_ctrl_name, 0, k->cc_key, pressed, pressed ? 1.0f : 0.0f, 0);
}

static void update_keys(void) {
  padUpdate(&pad);
  const u64 d = padGetButtons(&pad);
  for (unsigned i = 0; i < NUM_KEYMAP; i++) {
    const int now = (d & g_keymap[i].mask) ? 1 : 0;
    if (now != g_prev[i]) {
      send_button((int)i, now);
      g_prev[i] = now;
    }
  }

  // analog sticks -> controller axes (normalised -1..1). Only emit on a real
  // change past a small deadzone: re-sending centred axes every frame floods
  // the engine's controller listeners and can fight d-pad navigation.
  if (e_ctrlAxis) {
    HidAnalogStickState l = padGetStickPos(&pad, 0);
    HidAnalogStickState r = padGetStickPos(&pad, 1);
    const float dz = 0.12f; // ~3900/32768 deadzone
    float lx = l.x / 32768.0f, ly = -l.y / 32768.0f;
    float rx = r.x / 32768.0f, ry = -r.y / 32768.0f;
    lx = (lx > -dz && lx < dz) ? 0.0f : lx;
    ly = (ly > -dz && ly < dz) ? 0.0f : ly;
    rx = (rx > -dz && rx < dz) ? 0.0f : rx;
    ry = (ry > -dz && ry < dz) ? 0.0f : ry;

    // right_stick_mirror -- emulate the left joystick using the physical
    // right stick: whatever the right stick reports gets sent to the game
    // on the SAME channel/ids as the real left stick (CC_JOY_LX/CC_JOY_LY,
    // 1000/1001) -- the one channel confirmed live end-to-end (this is how
    // field movement already works). Right stick tilt takes priority over
    // the real left stick when both are active, since it's standing in for
    // it; with the right stick centred, the real left stick passes through
    // untouched. The right stick's own raw position is still also sent as
    // CC_JOY_RX/CC_JOY_RY below in case anything ever reads that separately,
    // but that's not what makes this feature work.
    float send_lx = lx, send_ly = ly;
    if (config.right_stick_mirror && (rx != 0.0f || ry != 0.0f)) {
      send_lx = rx;
      send_ly = ry;
    }

    const float axes[4] = { send_lx, send_ly, rx, ry };
    static const int axis_key[4] = { CC_JOY_LX, CC_JOY_LY, CC_JOY_RX, CC_JOY_RY };
    static float prev_axis[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
      if (axes[i] != prev_axis[i]) {
        e_ctrlAxis(fake_env, thiz, g_ctrl_name, 0, axis_key[i], axes[i], 1);
        prev_axis[i] = axes[i];
      }
    }
  }
}

// ---------------------------------------------------------------------------
// language -- map config.language to DeviceInfo's LocalizationLanguageType index
// (resources.bin ships all nine under Localize/<code>/)
// ---------------------------------------------------------------------------

// config.language == "default" auto-selects whatever language the console
// itself is set to (Settings > System > Language), same idea as most retail
// games' own "System default" option. setGetSystemLanguage returns the raw
// system LanguageCode straight from system settings -- unlike setGetLanguageCode,
// it isn't filtered against any application language-support list, so it's a
// direct read of "whatever the switch system is using". LanguageCode is a
// packed string (see switchbrew's Settings services wiki page), so it's run
// through setMakeLanguage to get the SetLanguage enum back out. Requires
// setInitialize() to have been called first (done once near the top of main()).
//
// Maps that enum down to one of the codes resources.bin actually ships
// (ja/en/de/it/es/fr/zh/zh-Hant/ko); regional variants collapse onto their
// base language (e.g. en-GB and es-419 both count as their base). Anything
// resources.bin doesn't have at all (Dutch, Portuguese, Russian, ...) falls
// back to English, same as an unrecognized config.language value would.
static const char *system_language_code(void) {
  u64 code = 0;
  if (R_FAILED(setGetSystemLanguage(&code)))
    return "en";

  SetLanguage lang;
  if (R_FAILED(setMakeLanguage(code, &lang)))
    return "en";

  switch (lang) {
    case SetLanguage_JA:     return "ja";
    case SetLanguage_FR:
    case SetLanguage_FRCA:   return "fr";
    case SetLanguage_DE:     return "de";
    case SetLanguage_IT:     return "it";
    case SetLanguage_ES:
    case SetLanguage_ES419:  return "es";
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS: return "zh";
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT: return "zh-Hant";
    case SetLanguage_KO:     return "ko";
    case SetLanguage_ENUS:
    case SetLanguage_ENGB:
    default:                 return "en"; // includes NL/PT/RU/PTBR -- unsupported
  }
}

static int lang_index(void) {
  const char *l = config.language;
  if (!strcmp(l, "default"))
    l = system_language_code();
  if (!strcmp(l, "ja")) return 0;
  if (!strcmp(l, "en")) return 1;
  if (!strcmp(l, "de")) return 2;
  if (!strcmp(l, "it")) return 3;
  if (!strcmp(l, "es")) return 4;
  if (!strcmp(l, "fr")) return 5;
  if (!strcmp(l, "zh") || !strcmp(l, "zh-Hans") || !strcmp(l, "zh_CN")) return 6;
  if (!strcmp(l, "zh-Hant") || !strcmp(l, "zh_TW")) return 7;
  if (!strcmp(l, "ko")) return 8;
  return 1; // default English
}
// Cached at startup; the language never changes at runtime so we don't need
// to re-run the strcmp chain every frame.
static int g_lang_idx = 1;
static void force_language(void) {
  if (e_lang_var) *e_lang_var = g_lang_idx;
}

// ---------------------------------------------------------------------------
// touch -- single pointer mapped into the engine's screen space
// ---------------------------------------------------------------------------

// Pre-allocated static arrays for the touch-move JNI call.  The engine only
// ever sees one touch point (single-finger), so these are always length-1.
// Using static storage avoids three malloc/free pairs (+ six mutex lock/unlock
// calls on the local-ref registry) every frame while a finger is held down.
static int   s_touch_ids[1]   = { 0 };
static float s_touch_xs[1]    = { 0 };
static float s_touch_ys[1]    = { 0 };
// Fake JNI array headers that point at the static data above.
// FakePriArray layout: tag, len, elem_size, data -- must match jni_fake.c.
typedef struct { uint32_t tag; int len; int elem_size; void *data; } TouchPriArray;
#define TAG_PRIARR_TOUCH 0x50415231u // 'PAR1', same as TAG_PRIARR in jni_fake.c
static TouchPriArray s_jids = { TAG_PRIARR_TOUCH, 1, 4, s_touch_ids };
static TouchPriArray s_jxs  = { TAG_PRIARR_TOUCH, 1, 4, s_touch_xs  };
static TouchPriArray s_jys  = { TAG_PRIARR_TOUCH, 1, 4, s_touch_ys  };

static int touch_down = 0;
static float last_tx = 0, last_ty = 0;

static void update_touch(void) {
  HidTouchScreenState st = {0};
  int have = hidGetTouchScreenStates(&st, 1) && st.count > 0;

  if (have) {
    // libnx reports panel coords in 1280x720; scale to our framebuffer
    const float sx = (float)screen_width / 1280.0f;
    const float sy = (float)screen_height / 720.0f;
    float x = st.touches[0].x * sx;
    float y = st.touches[0].y * sy;
    if (!touch_down) {
      touch_down = 1;
      if (e_touchBegin) e_touchBegin(fake_env, thiz, 0, x, y);
    } else if (e_touchMove) {
      // Update the static arrays in-place; no heap allocation needed.
      s_touch_xs[0] = x;
      s_touch_ys[0] = y;
      e_touchMove(fake_env, thiz, &s_jids, &s_jxs, &s_jys);
    }
    last_tx = x; last_ty = y;
  } else if (touch_down) {
    touch_down = 0;
    if (e_touchEnd) e_touchEnd(fake_env, thiz, 0, last_tx, last_ty);
  }
}

// ---------------------------------------------------------------------------
// DIAG_SCALE -- one-off instrumentation to pin down the exact scale factors
// in the cocos2d Director/GLView resolution pipeline, in order to diagnose
// the scroll-only shimmer bug. Reads fields directly off the live Director
// and GLView objects (offsets confirmed via static disassembly of this same
// libchrono.so build -- BuildID-matched). Pure diagnostic; no behavior
// change. Remove or set to 0 once the numbers are in.
// ---------------------------------------------------------------------------
#define DIAG_SCALE 0
#if DIAG_SCALE
static void diag_scale_dump(void) {
  static int done = 0;
  static u64 freq = 0, t0 = 0;
  static int sample = 0;
  const int MAX_SAMPLES = 12;   // ~1 every 3s for the first 36s of runtime
  if (done || !e_director_getInstance) return;
  if (!freq) { freq = armGetSystemTickFreq(); t0 = armGetSystemTick(); }
  const u64 now = armGetSystemTick();
  const double secs = (double)(now - t0) / (double)freq;
  if (sample == 0 && secs < 1.0) return;              // first sample ~1s after boot
  if (sample > 0 && secs < (double)sample * 3.0) return; // then every ~3s

  void *director = e_director_getInstance();
  if (!director) return;

  unsigned char *d = (unsigned char *)director;
  float contentScaleFactor = *(float *)(d + 0x188);
  float designW = *(float *)(d + 0x180);
  float designH = *(float *)(d + 0x184);
  void *glview = *(void **)(d + 0xf0);

  // Real physical panel size, straight from the OS -- independent of whatever
  // EGL surface size we asked for. If this doesn't match screen_width/height,
  // HOS's window compositor is rescaling our buffer onto the panel on every
  // frame, which is a scaling stage no libchrono.so patch can touch.
  s32 panel_w = 0, panel_h = 0;
  appletGetDefaultDisplayResolution(&panel_w, &panel_h);

  FILE *fl = fopen("scale_diag.log", "a");
  if (fl) {
    fprintf(fl, "[t=%.1fs] screen_width=%d screen_height=%d panelResolution=%dx%d "
                "contentScaleFactor=%.6f designResolution=%.2fx%.2f glview=%p\n",
            secs, screen_width, screen_height, panel_w, panel_h,
            contentScaleFactor, designW, designH, glview);
    if (glview) {
      unsigned char *g = (unsigned char *)glview;
      float frameW = *(float *)(g + 0x24);
      float frameH = *(float *)(g + 0x28);
      float dsgnW  = *(float *)(g + 0x2c);
      float dsgnH  = *(float *)(g + 0x30);
      float scaleX = *(float *)(g + 0x60);
      float scaleY = *(float *)(g + 0x64);
      int   policy = *(int   *)(g + 0x68);
      fprintf(fl, "         glview.frameSize=%.2fx%.2f glview.designSize=%.2fx%.2f "
                  "mScaleX=%.6f mScaleY=%.6f resolutionPolicy=%d\n",
              frameW, frameH, dsgnW, dsgnH, scaleX, scaleY, policy);
    }
    fclose(fl);
  }
  sample++;
  if (sample >= MAX_SAMPLES) done = 1;
}
#endif

// ---------------------------------------------------------------------------

int main(void) {
  cpu_boost(1);

  // Always rewrite config.txt after parsing it: read_config seeds defaults
  // for any key the file doesn't mention, but the file itself only gains
  // that key once we explicitly resave it. (config_needs_rewrite exists to
  // gate this but was never actually being set anywhere, so previously a
  // freshly-added Config field would silently use its default forever
  // without ever showing up in an existing config.txt -- worth knowing if
  // you've added other config vars before now and wondered why.)
  read_config(CONFIG_NAME);
  write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  set_screen_size(appletGetOperationMode());

  plInitialize(PlServiceType_User);
  // Needed by system_language_code() (config.language == "default") the first
  // time lang_index() runs, a few steps down during apply_game_patches().
  setInitialize();
  gfx_init();

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());

  // Spread our threads across every core the system grants this process. Launched
  // via a game override the Switch hands an application cores 0,1,2 (core 3 is
  // reserved for the OS). The loader starts the main thread with the full mask,
  // but we set it explicitly so the affinity is inherited by the threads created
  // afterwards -- notably mesa's glthread GL-submission worker -- letting the
  // render thread and the GL thread run on different cores instead of contending
  // for one. This is what lets a single-threaded engine keep up on stock clocks.
  {
    u64 cores = 0;
    if (R_SUCCEEDED(svcGetInfo(&cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && cores) {
      const int ideal = __builtin_ctzll(cores); // lowest granted core (valid ideal)
      svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal, 1ULL << ideal); // was: cores
    }
  }

  // Ask mesa to run GL command submission on a worker thread (glthread). The
  // engine renders single-threaded and spends much of each frame inside the
  // nouveau driver building command buffers; glthread moves that onto a spare
  // CPU core, shortening the per-frame critical path that otherwise needs a
  // CPU/EMC overclock to hold 60. Must be set before the GL context is created;
  // harmless no-op if this mesa build doesn't support it.
  if (config.gl_threaded)
    setenv("mesa_glthread", "true", 1);

  // Skip mesa's internal validation of GL calls (bad enum/state checks) before
  // they reach the driver -- cocos2d-x's calls are already well-formed, so this
  // just removes CPU bookkeeping on a path that fires constantly. No behavioral
  // effect on a correct caller; harmless no-op if this mesa build lacks it.
  if (config.gl_no_error)
    setenv("MESA_NO_ERROR", "1", 1);

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES 2.0 context.");
  // seed the resize fallback with the size that just worked
  s_egl_good_w = screen_width;
  s_egl_good_h = screen_height;

  // --- load both modules: libc++_shared first so libchrono's std imports bind ---
  if (so_load(&cpp_mod, SOCPP_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SOCPP_NAME);

  void *chrono_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + cpp_mod.load_size, 0x100000);
  size_t used = (uintptr_t)chrono_base - (uintptr_t)heap_so_base;
  if (so_load(&game_mod, SO_NAME, chrono_base, heap_so_limit - used) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  // relocate + resolve (libchrono's std::/__cxa_ symbols resolve into libc++_shared)
  ct_resolve_imports(&cpp_mod);
  ct_resolve_imports(&game_mod);

  // resolve every engine entry point now, while the symbol tables (in load_base)
  // are still readable -- so_finalize maps the code and locks load_base out.
  resolve_entry_points();
  if (!e_nativeInit || !e_nativeRender || !e_JNI_OnLoad)
    fatal_error("Could not resolve cocos2d-x engine entry points.");

  // Apply config-gated binary patches to libchrono.so while load_base is still
  // writable (before so_finalize() locks it to RX). Each group is independent
  // and skips safely if it encounters an unexpected instruction word, so a
  // future .so rebuild won't silently corrupt -- it just prints a warning.
  apply_game_patches(&game_mod);
  apply_palette_cache(&game_mod);

  // Force the UI/text language to config.language regardless of what the engine
  // detects. resources.bin contains all languages under Localize/<code>/; the
  // engine selects via DeviceInfo::mCurrentLanguage, read DIRECTLY by both
  // getCurrentLanguage() and getLocalizeResourcePath(). We can't reliably win
  // the timing race against the engine's own write, so we patch those two reads
  // to a constant (the config index). Done now, while load_base is writable.
  {
    const int idx = lang_index() & 0xffff;
    // getCurrentLanguage: ldr w0,[x8] @ +0x8  ->  movz w0, #idx
    uintptr_t gcl = so_find_addr(&game_mod, "_ZN10DeviceInfo18getCurrentLanguageEv");
    *(uint32_t *)(gcl + 0x8) = 0x52800000u | ((uint32_t)idx << 5);
    // getLocalizeResourcePath: ldrsw x9,[x9] @ +0x20  ->  movz x9, #idx
    uintptr_t glrp = so_find_addr(&game_mod, "_ZN10DeviceInfo23getLocalizeResourcePathEv");
    *(uint32_t *)(glrp + 0x20) = 0xd2800009u | ((uint32_t)idx << 5);
  }

  so_finalize(&cpp_mod);
  so_finalize(&game_mod);
  so_flush_caches(&cpp_mod);
  so_flush_caches(&game_mod);

  tls_setup_guard();

  // C++ static constructors: runtime first, then the game.
  so_execute_init_array(&cpp_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&cpp_mod);
  so_free_temp(&game_mod);

  // --- JNI + cocos bootstrap ---
  jni_init();
  thiz = jni_make_object("AppActivity");

  // writable / save directory = <game directory (CWD)>/saves
  //
  // Previously this was the bare CWD, so save files (common.bin, the cocos2d
  // prefs file, etc.) landed directly in /switch/ct/ alongside config.ini,
  // debug.log, and everything else. Saves now live in their own subfolder
  // next to mods/, so the top-level directory stays tidy.
  //
  // Pre-existing saves from the old (bare-CWD) location are migrated
  // automatically, once, the first time this runs -- see the migration
  // block right after jni_set_writable_path() below.
  static char wdir[512];
  char base[512];
  if (getcwd(base, sizeof(base)) && base[0]) {
    size_t n = strlen(base);
    if (n > 1 && base[n - 1] == '/') base[n - 1] = 0;
  } else {
    strcpy(base, ".");
  }

  int wrote = snprintf(wdir, sizeof(wdir), "%s/saves", base);
  if (wrote <= 0 || (size_t)wrote >= sizeof(wdir)) {
    // Path too long to fit -- extremely unlikely, but fall back to the
    // un-suffixed base dir rather than use a truncated/garbage path.
    strlcpy(wdir, base, sizeof(wdir));
  } else if (mkdir(wdir, 0777) != 0 && errno != EEXIST) {
    // Couldn't create saves/ (read-only fs, permissions, etc.) -- fall back
    // to the original behaviour (save directly in CWD) rather than risk the
    // engine writing into a directory that doesn't exist.
    debugPrintf("main: mkdir('%s') failed (errno=%d) -- using '%s' instead\n",
                wdir, errno, base);
    strlcpy(wdir, base, sizeof(wdir));
  }

  jni_set_writable_path(wdir);

  // One-time migration: move pre-existing saves from the old location
  // (bare CWD) into saves/, if they haven't been migrated already.
  //
  // Only files matching the engine's known save-file naming are moved:
  //   Chrono_sp_<N>_0.dat  -- per-slot save data (N can be any integer)
  //   meta.bin             -- save-slot metadata/index
  //   common.bin           -- common/shared save data
  // Everything else in the old directory (config.ini, debug.log, the .so
  // files, ChronoType.ttf, mods/, assets/, etc.) is left untouched.
  //
  // Skipped entirely if wdir == base (mkdir of saves/ failed above, so
  // saves are staying in the old location anyway -- nothing to migrate).
  if (strcmp(wdir, base) != 0) {
    DIR *mig_check = opendir(wdir);
    int saves_already_present = 0;
    if (mig_check) {
      struct dirent *me;
      while ((me = readdir(mig_check)) != NULL) {
        if (!strcmp(me->d_name, "common.bin") || !strcmp(me->d_name, "meta.bin") ||
            !strncmp(me->d_name, "Chrono_sp_", 10)) {
          saves_already_present = 1;
          break;
        }
      }
      closedir(mig_check);
    }

    if (!saves_already_present) {
      DIR *old_dir = opendir(base);
      if (old_dir) {
        struct dirent *e;
        int moved = 0, skipped = 0;
        while ((e = readdir(old_dir)) != NULL) {
          int is_save_file =
              !strcmp(e->d_name, "common.bin") ||
              !strcmp(e->d_name, "meta.bin") ||
              !strncmp(e->d_name, "Chrono_sp_", 10);
          if (!is_save_file) continue;

          char old_path[600], new_path[600];
          snprintf(old_path, sizeof(old_path), "%s/%s", base, e->d_name);
          snprintf(new_path, sizeof(new_path), "%s/%s", wdir, e->d_name);

          struct stat st;
          if (stat(new_path, &st) == 0) {
            // Something's already there (shouldn't happen given the guard
            // above) -- don't clobber it, just leave the old file in place.
            debugPrintf("main: migrate skip (dest exists): %s\n", e->d_name);
            skipped++;
            continue;
          }

          if (rename(old_path, new_path) == 0) {
            moved++;
          } else {
            debugPrintf("main: migrate failed for %s (errno=%d)\n",
                        e->d_name, errno);
            skipped++;
          }
        }
        closedir(old_dir);
        if (moved || skipped)
          debugPrintf("main: save migration -- moved %d, skipped %d\n",
                      moved, skipped);
      }
    }
  }

  {
    char prefs_path[600];
    snprintf(prefs_path, sizeof(prefs_path), "%s/Cocos2dxPrefsFile.txt", wdir);
    prefs_init(prefs_path);
  }

  jni_set_bitmap_cb((BitmapDCFn)e_bitmapDC);
  jni_set_video_cb((VideoCbFn)e_videoCb);
  jni_set_ime_cb((ImeInsertFn)e_insertText, (ImeDeleteFn)e_deleteBackward);
  jni_set_editbox_cb((EbBeginFn)e_ebDidBegin, (EbTextFn)e_ebChanged, (EbTextFn)e_ebDidEnd);
  movie_set_gl_invalidate(e_glInvalidate);
  cache_progress_set_gl_invalidate(e_glInvalidate);

  // a persistent device-name jstring for the controller event callbacks
  g_ctrl_name = jni_make_string("Nintendo Switch Controller");

  // force the UI language before the engine builds the title scene
  g_lang_idx = lang_index();
  force_language();
  setExit(); // done with system-language detection for good

  // JniHelper::setJavaVM + cocos_android_app_init (creates the AppDelegate)
  if (e_JNI_OnLoad)
    e_JNI_OnLoad(fake_vm, NULL);

  // hand the engine its context/assets/writable roots
  void *ctx  = jni_make_object("Context");
  void *amgr = jni_make_object("AssetManager");
  if (e_nativeSetContext) e_nativeSetContext(fake_env, thiz, ctx, amgr);
  if (e_nativeSetApkPath) { void *p = jni_make_string("game.apk"); e_nativeSetApkPath(fake_env, thiz, p); }
  if (e_setAssetManager) e_setAssetManager(fake_env, thiz, ctx, amgr);
  if (e_setExternalStorageInfo) {
    void *a = jni_make_string(wdir);
    void *b = jni_make_string(wdir);
    void *c = jni_make_string("com.square_enix.android_googleplay.chrono");
    e_setExternalStorageInfo(fake_env, thiz, a, b, c);
  }

  // create the GLView + run applicationDidFinishLaunching (the engine's entry)
  e_nativeInit(fake_env, thiz, screen_width, screen_height);


  // input
  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();
  for (unsigned i = 0; i < NUM_KEYMAP; i++) g_prev[i] = 0;
  init_button_remap(); // resolve key_zl/zr/plus/minus config -> g_remap[]
  // register controller 0 so Controller::getKeyStatus polling has a target
  if (e_ctrlConnected)
    e_ctrlConnected(fake_env, thiz, g_ctrl_name, 0);

  int paused = 0;
  int boot_frames = 0;
  AppletOperationMode cur_mode = appletGetOperationMode();
  while (appletMainLoop() && !jni_quit_requested) {
    // dock/undock: switch resolution live. appletGetOperationMode() just
    // reads a value libnx already keeps in sync from the focus/state-change
    // notifications appletMainLoop() pumps above, so polling it every frame
    // is the same cheap pattern as the appletGetFocusState() check below --
    // no extra IPC round trip.
    {
      const AppletOperationMode new_mode = appletGetOperationMode();
      if (new_mode != cur_mode) {
        cur_mode = new_mode;
        set_screen_size(cur_mode);
        if (egl_resize() && e_nativeOnSurfaceChanged)
          // tells cocos2d-x's GLView the physical frame size changed, so it
          // recomputes the viewport/scale factor against the (unchanged)
          // design resolution -- the same path Android uses for rotation.
          e_nativeOnSurfaceChanged(fake_env, thiz, screen_width, screen_height);
      }
    }

    // pause/resume on focus changes
    const int focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (!focused && !paused) { if (e_nativeOnPause) e_nativeOnPause(); paused = 1; }
    else if (focused && paused) { if (e_nativeOnResume) e_nativeOnResume(); paused = 0; }

    if (paused) {
      svcSleepThread(16000000ull); // ~16ms; don't spin while backgrounded
      continue;
    }

    update_keys();
    update_touch();
    force_language(); // keep mCurrentLanguage pinned for any direct readers
                      // (getCurrentLanguage/getLocalizeResourcePath are patched)

    // PlayMovieScene::update() auto-advances only when InvaildDevice::getKeyRelease
    // returns a non-zero bit -- but our stub always returns 0, so the scene never
    // transitions by itself. Instead, after movie_hold_last_frame drains, we inject
    // a synthetic BACK press (AK_BACK=4 -> cocos KeyCode 6 = KEY_ESCAPE/KEY_BACK),
    // which is exactly what PlayMovieScene::onKeyboardPressed checks before calling
    // SceneManager::NextScene(-1). This runs BEFORE nativeRender so the key event
    // is processed inside the engine's normal frame, safely outside the init() stack.
    if (movie_take_completion_flag() && e_keyEvent) {
      e_keyEvent(fake_env, thiz, AK_BACK, 1); // key down
      e_keyEvent(fake_env, thiz, AK_BACK, 0); // key up
    }

#if DEBUG_INSTR
    static u64 fi_freq2 = 0;
    if (!fi_freq2) fi_freq2 = armGetSystemTickFreq();
    const u64 t_render_start = armGetSystemTick();
#endif
    palette_cache_frame();
    e_nativeRender(fake_env);
#if DEBUG_INSTR
    const u64 t_render_end = armGetSystemTick();
#endif
    // TEMPORARY -- drains the scroll-centering diagnostic snapshot that
    // apply_field_scroll_debug_log() (patches.h) writes into homebrew
    // memory every time FieldMap::Scroll() runs. Safe to call every frame
    // even when the hook isn't installed (s_scroll_dbg_snap.seq just stays
    // 0 forever, so nothing prints). Remove alongside the patches.h hook
    // once the centering drift is diagnosed.
    scroll_dbg_drain();
    // If a movie just finished, movie_post_render presents the last video frame
    // in place of whatever the engine drew this cycle (which may be black during
    // the scene transition). Once the hold count is exhausted it restores GL
    // state and hands control back. On normal frames it's a no-op (returns 0).
    if (!movie_post_render(s_display, s_surface))
      eglSwapBuffers(s_display, s_surface);
#if DEBUG_INSTR
    const u64 t_present_end = armGetSystemTick();
    const u64 render_us  = (t_render_end  - t_render_start) * 1000000ull / fi_freq2;
    const u64 present_us = (t_present_end - t_render_end)   * 1000000ull / fi_freq2;
#endif

    jni_ime_service(); // show swkbd for a pending EditBox, outside nativeRender

    if (boot_frames < 10 && ++boot_frames == 10)
      cpu_boost(0);

#if DIAG_SCALE
    diag_scale_dump();
#endif

#if DEBUG_INSTR
    // bucket frame durations; dump a one-line summary every ~5s. A high
    // over16ms/over33ms count while walking diagonally => real presentation
    // hitches (GPU/streaming bound), which delta pacing cannot hide.
    {
      static u64 fi_freq = 0, fi_last = 0, fi_t0 = 0;
      static u32 fi_frames = 0, fi_o16 = 0, fi_o33 = 0, fi_max_us = 0;
      // NEW: separate accumulators so we can see WHICH half of the frame
      // (engine render vs present/vsync-wait) actually owns the time --
      // a core/GPU-load overlay can't distinguish "busy" from "blocked in
      // WaitSynchronization", so this measures it directly instead.
      static u64 fi_render_sum = 0, fi_present_sum = 0;
      static u32 fi_render_max = 0, fi_present_max = 0;
      if (!fi_freq) { fi_freq = armGetSystemTickFreq(); fi_last = fi_t0 = armGetSystemTick(); }
      const u64 nowt = armGetSystemTick();
      const u64 us = (nowt - fi_last) * 1000000ull / fi_freq;
      fi_last = nowt;
      fi_frames++;
      if (us > 17000) fi_o16++;            // overran one vsync
      if (us > 34000) fi_o33++;            // dropped a whole frame
      if (us > fi_max_us) fi_max_us = (u32)us;
      fi_render_sum  += render_us;
      fi_present_sum += present_us;
      if ((u32)render_us  > fi_render_max)  fi_render_max  = (u32)render_us;
      if ((u32)present_us > fi_present_max) fi_present_max = (u32)present_us;
      if ((nowt - fi_t0) * 1000ull / fi_freq >= 5000) {
        FILE *fl = fopen("frametime.log", "a");
        if (fl) {
          const double secs = (double)(nowt - fi_t0) / (double)fi_freq;
          fprintf(fl, "frames=%u avg=%.2fms max=%.1fms over17ms=%u over34ms=%u "
                      "render_avg=%.2fms render_max=%.1fms present_avg=%.2fms present_max=%.1fms\n",
                  fi_frames, fi_frames ? secs * 1000.0 / fi_frames : 0.0,
                  fi_max_us / 1000.0, fi_o16, fi_o33,
                  fi_frames ? (double)fi_render_sum  / fi_frames / 1000.0 : 0.0,
                  fi_render_max / 1000.0,
                  fi_frames ? (double)fi_present_sum / fi_frames / 1000.0 : 0.0,
                  fi_present_max / 1000.0);
          fclose(fl);
        }
        fi_frames = fi_o16 = fi_o33 = fi_max_us = 0; fi_t0 = nowt;
        fi_render_sum = fi_present_sum = 0; fi_render_max = fi_present_max = 0;
      }
    }
#endif
  }

  prefs_flush();
  if (e_nativeOnPause) e_nativeOnPause();
  opensles_shutdown();
  egl_deinit();
  plExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
