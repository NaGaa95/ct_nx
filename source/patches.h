/* patches.h -- runtime ARM64 instruction patches for libchrono.so
 *
 * Applied at boot to load_base (writable RW mapping) before so_finalize()
 * locks it to RX. Identical in effect to the offline Python patcher scripts,
 * but driven by config.ini so individual fixes can be toggled without
 * re-patching the .so on disk.
 *
 * Three feature groups, each gated by a config flag:
 *
 *   cursor_fix         -- keep selected-item text WHITE and use a dark
 *                         semi-transparent highlight instead of the cream
 *                         colour-inversion look (remove_cursor_invert.py v9)
 *
 *   remove_bilinear    -- force GL_NEAREST (pixel-perfect point sampling)
 *                         everywhere instead of bilinear filtering
 *                         (remove_bilinear.py)
 *
 *   remove_mobile_ui   -- hide the on-screen touch-overlay buttons: field
 *                         menu button, world-map menu/map/warp buttons, all
 *                         five right-side title-screen icon buttons
 *                         (remove_mobile_ui.py v8), the back/close touch
 *                         buttons drawn in the bottom-right corner of every
 *                         menu-system status bar (Equip/Item/Tech/Config/
 *                         SaveLoad/Formation, and the top-level Main Menu
 *                         bar) -- letting those bars reuse their own
 *                         existing full-width "no button" layout instead of
 *                         leaving a gap (remove_menu_buttons.py v1, folded
 *                         into this same feature/config flag) -- AND the
 *                         on-screen touch buttons built by
 *                         SceneSpecialRace::setMenu for Johnny's motorcycle
 *                         race minigame (every button the function creates,
 *                         across both of its sub-race layouts) -- AND the
 *                         green/yellow/red/blue color-button prompts for
 *                         dance/QTE scenes, via VirtualPad::openVPad's
 *                         setVisible toggle. Functional cursor/passcode
 *                         controls remain visible; construction is untouched.
 *
 * Every patch entry records the old word so we can verify the .so matches
 * the expected build before writing anything. A mismatch prints a warning
 * and skips that entry -- it never silently corrupts.
 *
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __PATCHES_H__
#define __PATCHES_H__

#include <stdint.h>
#include "so_util.h"
#include "util.h"

// ---------------------------------------------------------------------------
// Patch descriptor
//
// sym_name: exported symbol whose load_base address anchors the patch.
//           NULL means use raw_vaddr directly (for functions with no exported
//           symbol -- the world-button helper, cave sites, etc.).
// func_off:  byte offset from the symbol's load_base address to the word.
//            Ignored when sym_name is NULL.
// raw_vaddr: link-time virtual address used when sym_name is NULL.
// old_word:  expected current instruction (LE uint32). 0 = "don't check"
//            (used for cave slots that are verified-zero in the .so).
// new_word:  replacement instruction.
// desc:      human-readable label for debug output.
// ---------------------------------------------------------------------------
typedef struct {
  const char   *sym_name;
  uint32_t      func_off;
  uint32_t      raw_vaddr;
  uint32_t      old_word;
  uint32_t      new_word;
  const char   *desc;
} PatchEntry;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Compute the writable load_base address for a patch entry.
// Returns 0 if the symbol is required but missing (caller skips).
static uintptr_t patch_addr(so_module *mod, const PatchEntry *p) {
  if (p->sym_name) {
    // Symbol-relative: load_base + symbol_value + func_off
    for (int i = 0; i < mod->num_syms; i++) {
      const char *name = mod->dynstrtab + mod->syms[i].st_name;
      if (__builtin_strcmp(name, p->sym_name) == 0)
        return (uintptr_t)mod->load_base + mod->syms[i].st_value + p->func_off;
    }
    debugPrintf("patches: symbol not found: %s\n", p->sym_name);
    return 0;
  } else {
    // Raw vaddr: subtract load_virtbase, add load_base (same as so_find_addr logic)
    return (uintptr_t)mod->load_base + p->raw_vaddr;
  }
}

// Apply one patch. Prints a diagnostic on mismatch but never aborts.
static void apply_patch(so_module *mod, const PatchEntry *p) {
  uintptr_t addr = patch_addr(mod, p);
  if (!addr) return;

  uint32_t cur;
  __builtin_memcpy(&cur, (void *)addr, 4);

  if (cur == p->new_word) {
    // Already at the desired value -- idempotent, no write needed.
    return;
  }
  if (p->old_word && cur != p->old_word) {
    debugPrintf("patches: MISMATCH @ %s+0x%x / vaddr 0x%x: "
                "expected %08x got %08x -- skipping (%s)\n",
                p->sym_name ? p->sym_name : "(raw)",
                p->sym_name ? p->func_off : p->raw_vaddr,
                p->raw_vaddr, p->old_word, cur, p->desc);
    return;
  }

  __builtin_memcpy((void *)addr, &p->new_word, 4);
  debugPrintf("patches: %08x -> %08x  %s\n", cur, p->new_word, p->desc);
}

// Apply an array of patches.
static void apply_patches(so_module *mod, const PatchEntry *table, int count) {
  for (int i = 0; i < count; i++)
    apply_patch(mod, &table[i]);
}

#define PATCH_COUNT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

// ---------------------------------------------------------------------------
// Macro helpers for the patch tables
// ---------------------------------------------------------------------------

// Symbol-relative entry: sym at func_off, expected old -> new
#define P_SYM(sym, off, old, new, desc) \
  { (sym), (uint32_t)(off), 0, (uint32_t)(old), (uint32_t)(new), (desc) }

// Raw-vaddr entry: no symbol, raw virtual address, old -> new
// old=0 means "don't verify" (used for cave zero-slots).
#define P_RAW(vaddr, old, new, desc) \
  { NULL, 0, (uint32_t)(vaddr), (uint32_t)(old), (uint32_t)(new), (desc) }

// Cave slot: zero-initialised padding, no old-word check needed.
#define P_CAVE(vaddr, new, desc) \
  { NULL, 0, (uint32_t)(vaddr), 0, (uint32_t)(new), (desc) }

// ---------------------------------------------------------------------------
// 1.  cursor_fix  (remove_cursor_invert.py v9)
//
//     Selected-item text: BLACK -> WHITE on all menus
//     Selection highlight: cream (0xfe,0xff,0xdd,0xcc) -> dark semi-transparent (0,0,0,0xa0)
//     Dialogue choice NON-selected bg: normal opacity -> dimmed (cave at 0xaa56a0)
// ---------------------------------------------------------------------------
static const PatchEntry g_cursor_patches[] = {

  // 1. BattleTechMenu::setListButtonFontColors(int)  selected text BLACK->WHITE
  P_RAW(0x6e40cc, 0xf946594a, 0xf946914a,
    "BattleTechMenu::setListButtonFontColors selected text BLACK->WHITE"),

  // 2. BattleItemMenu::onSelectChanged(int)  crash fix + WHITE
  P_RAW(0x6e52dc, 0x79002be9, 0x79002bea,
    "BattleItemMenu::onSelectChanged r,g BLACK->WHITE (strh w9->w10)"),
  P_RAW(0x6e52e0, 0x39005be8, 0x39005bea,
    "BattleItemMenu::onSelectChanged b BLACK->WHITE (strb w8->w10)"),

  // 3. MsgWindow::createMenu  dialogue choice windows
  P_RAW(0x5fd998, 0xaa1703e0, 0xaa1503e0,
    "MsgWindow::createMenu ItemSpriteA selectedImg bg WHITE->BLACK"),
  P_RAW(0x5fd9e8, 0xaa1703e0, 0xaa1503e0,
    "MsgWindow::createMenu ItemSpriteB normalImg bg WHITE->BLACK"),
  P_RAW(0x5fda18, 0xaa1503e1, 0xaa1703e1,
    "MsgWindow::createMenu selected-choice text tint BLACK->WHITE"),

  // 4. FieldMenu::init()  selected-state sprite colour BLACK->WHITE
  P_RAW(0x755538, 0xf9465908, 0xf9469108,
    "FieldMenu::init selected-state sprite colour BLACK->WHITE"),

  // 5. WorldMenu button helper (MENU/MAP/WARP)  selected-state BLACK->WHITE
  P_RAW(0x782ca4, 0xf9465929, 0xf9469129,
    "WorldMenu button helper selected-state sprite colour BLACK->WHITE"),

  // 6. BattleMenu::drawCommandWindow(int,bool,...)  selected text BLACK->WHITE
  P_RAW(0x5b56f0, 0xf946594a, 0xf946914a,
    "BattleMenu::drawCommandWindow(int,bool) selected text BLACK->WHITE"),

  // 7. BattleMenu::drawCommandWindow()  selected cmd text BLACK->WHITE
  P_RAW(0x5b2210, 0xf9465842, 0xf9469042,
    "BattleMenu::drawCommandWindow() selected cmd text BLACK->WHITE"),

  // 8. BattleListMenu highlight-colour: cream -> dark semi-transparent
  P_RAW(0x6e6994, 0x52801fc1, 0x52800001,
    "BattleListMenu highlight r 0xfe->0x00"),
  P_RAW(0x6e6998, 0x52801fe2, 0x52800002,
    "BattleListMenu highlight g 0xff->0x00"),
  P_RAW(0x6e699c, 0x52801ba3, 0x52800003,
    "BattleListMenu highlight b 0xdd->0x00"),
  P_RAW(0x6e69a0, 0x52801984, 0x52801404,
    "BattleListMenu highlight a 0xcc->0xa0"),

  // 9. BattleMenu::init() cmd-window SELECTED-FILL colour (2 sites)
  P_RAW(0x5ad8b0, 0x52801fc1, 0x52800001,
    "cmd-window fill #0x5ad8b0 r 0xfe->0x00"),
  P_RAW(0x5ad8b4, 0x52801fe2, 0x52800002,
    "cmd-window fill #0x5ad8b0 g 0xff->0x00"),
  P_RAW(0x5ad8b8, 0x52801ba3, 0x52800003,
    "cmd-window fill #0x5ad8b0 b 0xdd->0x00"),
  P_RAW(0x5ad8bc, 0x52801984, 0x52801404,
    "cmd-window fill #0x5ad8b0 a 0xcc->0xa0"),
  P_RAW(0x5ada68, 0x52801fc1, 0x52800001,
    "cmd-window fill #0x5ada68 r 0xfe->0x00"),
  P_RAW(0x5ada6c, 0x52801fe2, 0x52800002,
    "cmd-window fill #0x5ada68 g 0xff->0x00"),
  P_RAW(0x5ada70, 0x52801ba3, 0x52800003,
    "cmd-window fill #0x5ada68 b 0xdd->0x00"),
  P_RAW(0x5ada74, 0x52801984, 0x52801404,
    "cmd-window fill #0x5ada68 a 0xcc->0xa0"),

  // 10. MsgWindow::createMenu  dialogue NON-selected bg: dim via cave
  //     Route normal-bg makeColorNode call through cave at 0xaa56a0
  //     that enables opacity cascade then setOpacity(160).
  P_RAW(0x5fd950, 0x940000b5, 0x94129f54,
    "dialogue normal bg makeColorNode -> cascade+dim cave"),

  // Cave @ 0xaa56a0 (zero-padding region; BL offset from 0x5fd950 verified above)
  P_CAVE(0xaa56a0, 0xa9bf7bf3, "cave: stp x19,x30,[sp,#-0x10]!"),
  P_CAVE(0xaa56a4, 0x97ed6160, "cave: bl #0x5fdc24 (makeColorNode)"),
  P_CAVE(0xaa56a8, 0xaa0003f3, "cave: mov x19,x0"),
  P_CAVE(0xaa56ac, 0x52800021, "cave: mov w1,#1"),
  P_CAVE(0xaa56b0, 0x97f2768f, "cave: bl #0x7430ec (setCascadeOpacityEnabledRecursive)"),
  P_CAVE(0xaa56b4, 0xaa1303e0, "cave: mov x0,x19"),
  P_CAVE(0xaa56b8, 0x52801401, "cave: mov w1,#0xa0 (opacity 160)"),
  P_CAVE(0xaa56bc, 0xf9400268, "cave: ldr x8,[x19]"),
  P_CAVE(0xaa56c0, 0xf9424908, "cave: ldr x8,[x8,#0x490] (setOpacity vtable slot)"),
  P_CAVE(0xaa56c4, 0xd63f0100, "cave: blr x8"),
  P_CAVE(0xaa56c8, 0xaa1303e0, "cave: mov x0,x19"),
  P_CAVE(0xaa56cc, 0xa8c17bf3, "cave: ldp x19,x30,[sp],#0x10"),
  P_CAVE(0xaa56d0, 0xd65f03c0, "cave: ret"),
};

// ---------------------------------------------------------------------------
// 2.  remove_bilinear  (remove_bilinear.py)
//
//     Force GL_NEAREST in initWithMipmaps and setAntiAliasTexParameters.
//     Uses symbol-relative addressing for correctness -- the two functions
//     have exported symbols in dynsym, so we look them up rather than
//     hardcoding load-time vaddrs that could shift if the .so is ever rebuilt.
// ---------------------------------------------------------------------------
static const PatchEntry g_bilinear_patches[] = {

  // cocos2d::Texture2D::initWithMipmaps(...)
  P_SYM("_ZN7cocos2d9Texture2D15initWithMipmaps",
        0, /* func_off is unused; we scan within the function below */
        0, 0, "(sentinel -- see note)"),
  // Note: remove_bilinear.py searches for instruction patterns within the
  // function body rather than at fixed offsets, because the patterns appear
  // at different offsets depending on optimisation. We replicate that scan
  // here with the same known old->new word pairs. Raw vaddrs work equally
  // well because this is a fixed build; symbol-relative are used for the
  // first entry only as a build-match guard (see apply_bilinear_patches).
  P_RAW(0, 0x1A980708, 0x2A1803E8,
    "initWithMipmaps MIN(mipmap) cinc->mov w8,w24 (always NEAREST)"),
  P_RAW(0, 0x1A9C0789, 0x2A1C03E9,
    "initWithMipmaps MIN cinc->mov w9,w28 (always NEAREST)"),
  P_RAW(0, 0x1A9C0782, 0x2A1C03E2,
    "initWithMipmaps MAG cinc->mov w2,w28 (always NEAREST)"),
  P_RAW(0, 0x5284C02A, 0x5284C00A,
    "initWithMipmaps VolatileMgr restore GL_LINEAR->GL_NEAREST"),
  P_RAW(0, 0x5284E029, 0x5284E009,
    "initWithMipmaps VolatileMgr restore GL_LINEAR_MIPMAP->GL_NEAREST_MIPMAP"),

  // cocos2d::Texture2D::setAntiAliasTexParameters()
  P_RAW(0, 0x5284E035, 0x5284E015,
    "setAntiAliasTexParameters MIN GL_LINEAR_MIPMAP_NEAREST->GL_NEAREST_MIPMAP_NEAREST"),
  P_RAW(0, 0x5284C036, 0x5284C016,
    "setAntiAliasTexParameters MIN GL_LINEAR->GL_NEAREST"),
  P_RAW(0, 0x5284C022, 0x5284C002,
    "setAntiAliasTexParameters MAG GL_LINEAR->GL_NEAREST"),
};

// ---------------------------------------------------------------------------
// 3.  remove_mobile_ui  (remove_mobile_ui.py v8 + remove_menu_buttons.py v1)
//
//     Fix 1: FieldMenu::setMenuAvailable -- field button always hidden,
//             VirtualController::setActive (Start key) still works
//     Fix 2: world-map MENU/MAP/WARP buttons -- setPosition->setVisible(false)
//     Fix 3: right-side title icon buttons -- hide+setSkip via cave at 0xaa6270
//     Fix 4: menu-system status bar back/close buttons (Equip/Item/Tech/
//             Config/SaveLoad/Formation, and the top Main Menu bar) --
//             see Fix 4's own comment below for the full derivation
//     Fix 5: SceneSpecialRace::setMenu -- on-screen touch buttons for
//             Johnny's motorcycle race minigame -- see Fix 5's own comment
//             below for the full derivation
//     Fix 6: VirtualPad::openVPad -- hide the redundant color-button Menu
//             for dance/QTE scenes. Keep the functional cursor/passcode
//             container visible and preserve construction and input handling.
//     Fix 7: cSfcWork::SetDefaultConfig (+ its two InitNewGameData /
//             InitNewGamePlusData siblings, all three inline the same
//             CONFIG_WORK constant) -- CONFIG_WORK.Movement assumed-default
//             RUN(0) -> WALK(1). Data-only, one rodata word. Does NOT touch
//             FieldImpl::atel_isDash's Walk(1)/Run(2) branches, so an
//             explicit user choice in the Settings menu is still honored
//             exactly as before -- see Fix 7's own comment below for the
//             full derivation
// ---------------------------------------------------------------------------

// Fix 4 targets: both overloads of nsMenu::StatusBar::init are exported in
// .dynsym, so (like remove_bilinear above) we anchor on the symbol rather
// than a hardcoded raw vaddr -- more robust to a future rebuild shifting
// code around.
#define STATUSBAR_INIT4_SYM \
  "_ZN6nsMenu9StatusBar4initERKNSt6__ndk112basic_stringIcNS1_11char_" \
  "traitsIcEENS1_9allocatorIcEEEEbRKNS1_8functionIFvPN7cocos2d3RefEEEESH_"
#define STATUSBAR_INIT5_SYM STATUSBAR_INIT4_SYM "b"

// Fix 5 target: SceneSpecialRace::setMenu, also exported in .dynsym --
// anchored on the symbol for the same reason as Fix 4 above.
#define RACE_SETMENU_SYM "_ZN16SceneSpecialRace7setMenuEPN7cocos2d4NodeE"

// Fix 6 target: VirtualPad::openVPad, exported in .dynsym -- anchored on
// the symbol for the same reason as Fix 4/5 above.
#define VPAD_OPENVPAD_SYM "_ZN10VirtualPad8openVPadEv"

static const PatchEntry g_mobile_ui_patches[] = {

  // Fix 1: FieldMenu::setMenuAvailable(bool)
  //   +24: STRB WZR,[X0,#0x330] -> MOV W1,WZR  (force setVisible(false))
  P_SYM("_ZN9FieldMenu16setMenuAvailableEb", 24,
        0x390CC01F, 0x2A1F03E1,
        "FieldMenu::setMenuAvailable +24 field button always hidden, Start untouched"),

  // Fix 2: world-button-builder helper @ 0x782C7C (no exported symbol)
  //   setPosition(0xC8) -> setVisible(0x170), bool arg = false, two paths
  P_RAW(0x782DC0, 0xF9406508, 0xF940B908,
    "world-button helper +0x144 vtable setPosition->setVisible [path A]"),
  P_RAW(0x782DC8, 0x1E232821, 0x2A1F03E1,
    "world-button helper +0x14C bool arg=false [path A]"),
  P_RAW(0x782DF8, 0x1E261001, 0x2A1F03E1,
    "world-button helper +0x17C bool arg=false [path B]"),
  P_RAW(0x782DFC, 0xF9406508, 0xF940B908,
    "world-button helper +0x180 vtable setPosition->setVisible [path B]"),

  // Fix 3a: setupMenuNodes hide range-check -> NOP (hide every right-side icon)
  P_RAW(0x77FE94, 0x54FFE1A8, 0xD503201F,
    "setupMenuNodes B.HI->NOP: hide all right-side icons unconditionally"),

  // Fix 3b: repurpose hide sequence -> cave call (setVisible+setSkip)
  P_RAW(0x77FE98, 0xF9400348, 0xAA1A03E0,
    "setupMenuNodes LDR X8,[X26]->MOV X0,X26 (State* arg for cave)"),
  P_RAW(0x77FE9C, 0xF9400908, 0x940C98F5,
    "setupMenuNodes LDR X8,[X8,#0x10]->BL 0xAA6270 (setVisible+setSkip cave)"),
  P_RAW(0x77FEA0, 0xAA1A03E0, 0x17FFFF0A,
    "setupMenuNodes MOV X0,X26->B #0x77FAC8 (loop continue)"),

  // Fix 3b cave @ 0xAA6270  (96B zero-padding; distinct from cursor cave 0xAA56A0)
  P_CAVE(0xAA6270, 0xA9BF7BF3, "cave: stp x19,x30,[sp,#-0x10]!"),
  P_CAVE(0xAA6274, 0xAA0003F3, "cave: mov x19,x0"),
  P_CAVE(0xAA6278, 0xF9400008, "cave: ldr x8,[x0]"),
  P_CAVE(0xAA627C, 0xF9400908, "cave: ldr x8,[x8,#0x10] getNode"),
  P_CAVE(0xAA6280, 0xD63F0100, "cave: blr x8"),
  P_CAVE(0xAA6284, 0xF9400008, "cave: ldr x8,[x0]"),
  P_CAVE(0xAA6288, 0xF940B908, "cave: ldr x8,[x8,#0x170] setVisible"),
  P_CAVE(0xAA628C, 0x2A1F03E1, "cave: mov w1,wzr"),
  P_CAVE(0xAA6290, 0xD63F0100, "cave: blr x8"),
  P_CAVE(0xAA6294, 0xAA1303E0, "cave: mov x0,x19"),
  P_CAVE(0xAA6298, 0x52800021, "cave: mov w1,#1"),
  P_CAVE(0xAA629C, 0x940293D9, "cave: bl 0xB4B200 setSkip(true)"),
  P_CAVE(0xAA62A0, 0xA8C17BF3, "cave: ldp x19,x30,[sp],#0x10"),
  P_CAVE(0xAA62A4, 0xD65F03C0, "cave: ret"),

  // Fix 3c: RET-stub UpdateIconVisible so it can never re-show icons
  P_RAW(0x780CC8, 0xA9BD7BFD, 0xD65F03C0,
    "UpdateIconVisible RET-stub: icons stay hidden"),

  // -------------------------------------------------------------------------
  // Fix 4: menu-system status bar back/close buttons (remove_menu_buttons.py)
  //
  //     Targets the menu SYSTEM screens (Equip, Item, Tech, Config,
  //     SaveLoad, Formation, and the top-level Main Menu bar) instead of
  //     the field/world-map/title-screen overlays Fixes 1-3 handle. All of
  //     those screens share one reusable header class, nsMenu::StatusBar,
  //     built via one of two overloads of nsMenu::StatusBar::init(...):
  //
  //       * 4-arg overload  (title, bool, backCallback, closeCallback)
  //           -- used by MenuNodeEquip/Item/Tech/Config/SaveLoad/Formation
  //       * 5-arg overload  (..., isTopBar)
  //           -- same body plus an isTopBar-style branch; patched
  //              defensively even though every verified caller resolves
  //              to the 4-arg one
  //
  //     Disassembly-confirmed structure shared by both overloads:
  //
  //         ldr  x8, [x20, #0x320]      ; backCallback std::function target
  //         cbz  x8, <no_back_button>   ; null -> skip creating back button
  //         ...                         ; non-null -> createBackButton(...),
  //                                     ;   subtracting its width from bar
  //         <no_back_button>:
  //         mov  w8, #0x43f00000        ; float 480.0 -- full design width,
  //         fmov s0, w8                 ;   used as the bar width instead
  //
  //         ldr  x8, [x20, #0x350]      ; closeCallback std::function target
  //         cbz  x8, <no_close_button>  ; null -> skip creating close button
  //         ...                         ; non-null -> createCloseButton(...),
  //                                     ;   same width-subtraction pattern
  //         <no_close_button>:
  //         ...                         ; falls through with whichever
  //                                     ;   width was computed above
  //
  //     That "no button, full-width bar" path already exists and already
  //     runs whenever a caller passes an empty std::function for either
  //     callback -- it is not new logic. This forces the CBZ gate to
  //     always take the skip branch, for every StatusBar, regardless of
  //     what the caller passed, by converting each CBZ into an
  //     unconditional B to the exact same target the CBZ already pointed
  //     at (the branch target is link-time-relative, so translating the
  //     whole .so to a different runtime load_base -- as so_util does --
  //     does not change it).
  //
  //     Net effect: no menu screen ever creates a back or close button
  //     node, and the Main Menu / location / time / money bars resize to
  //     the full region instead of leaving a gap where the button used to
  //     sit. Controller "cancel" input is untouched: these button nodes
  //     are purely visual touch targets wired to their own std::function
  //     callbacks; nothing else in the input pipeline depends on their
  //     existence. Verified safe to always-skip: StatusBar's constructor
  //     zero-initializes both the backButton/closeButton node-pointer
  //     fields and the backCallback/closeCallback std::function slots, and
  //     both StatusBar::setInteractive and the destructor null-check the
  //     button pointers before touching them -- so never allocating the
  //     nodes is a clean, well-defined state, not a dangling-pointer risk.
  // -------------------------------------------------------------------------

  // 4-arg overload: back-button gate @ +0x160 (CBZ X8,+0x40 -> B +0x40)
  P_SYM(STATUSBAR_INIT4_SYM, 0x160, 0xB4000208, 0x14000010,
    "StatusBar::init(4-arg) back button: force existing 'no button' skip "
    "branch (was conditional on backCallback == null)"),

  // 4-arg overload: close-button gate @ +0x258 (CBZ X8,+0xF0 -> B +0xF0)
  P_SYM(STATUSBAR_INIT4_SYM, 0x258, 0xB4000788, 0x1400003C,
    "StatusBar::init(4-arg) close button: force existing 'no button' skip "
    "branch (was conditional on closeCallback == null)"),

  // 5-arg overload: back-button gate @ +0x17C (CBZ X8,+0x40 -> B +0x40)
  P_SYM(STATUSBAR_INIT5_SYM, 0x17C, 0xB4000208, 0x14000010,
    "StatusBar::init(5-arg) back button: force existing 'no button' skip "
    "branch"),

  // 5-arg overload: close-button gate @ +0x2D8 (CBZ X8,+0x154 -> B +0x154)
  P_SYM(STATUSBAR_INIT5_SYM, 0x2D8, 0xB4000AA8, 0x14000055,
    "StatusBar::init(5-arg) close button: force existing 'no button' skip "
    "branch"),

  // -------------------------------------------------------------------------
  // Fix 5: SceneSpecialRace::setMenu -- Johnny's motorcycle race on-screen
  //         touch buttons
  //
  //     SceneSpecialRace::setMenu(cocos2d::Node* menuParent) is exported in
  //     .dynsym (size 2076 bytes) and is the sole place this scene builds
  //     its touch-button overlay -- confirmed by an inlined literal string
  //     "button" built via movz/movk immediates near the top of the
  //     function, and by every button-creation block ending in the exact
  //     same two-call pattern already proven out in Fix 2/3 above:
  //
  //         ldr  x8, [x8, #0xc8]     ; vtable slot: cocos2d::Node::setPosition
  //         ...                      ; s0/s1 loaded with float x/y immediates
  //         blr  x8
  //
  //     A run-time flag at this+0xc20 (set earlier in ::init from a race
  //     sub-mode ID) selects between two layouts:
  //
  //       * sub-mode flag set:   builds 2 buttons (fields this+0x5440,
  //                               this+0x5448)
  //       * sub-mode flag clear: builds 4 buttons -- one anonymous button
  //                               (only ever pushed into the button vector,
  //                               never stored to a named field) plus the
  //                               SAME this+0x5440 / this+0x5448 fields
  //                               (rebuilt with different on-screen
  //                               coordinates for this layout) plus a 4th
  //                               button at this+0x5450
  //
  //     Both layouts funnel into a shared tail that wraps everything built
  //     so far into a button container (this+0x5458) and adds it to
  //     menuParent -- we deliberately do NOT touch that container call
  //     (unconfirmed vtable slot 0x98, not the well-established 0xC8/0x170
  //     Node pair used everywhere else in this file); patching the 6
  //     individual button-creation sites below hides every button in both
  //     layouts without needing to touch that unverified call.
  //
  //     Same technique as Fix 2's world-map buttons: convert the
  //     setPosition(x,y) call into setVisible(false) --
  //
  //         ldr  x8, [x8, #0xc8]   ->   ldr  x8, [x8, #0x170]   (setVisible)
  //         fmov s1, w10           ->   mov  w1, wzr             (bool=false)
  //         blr  x8
  //
  //     The overwritten fmov/fadd instruction in each pair only ever fed
  //     the now-unused s0/s1 position registers -- verified dead once the
  //     call becomes setVisible(bool) instead of setPosition(float,float).
  //     Where the "this" pointer for the virtual call is set up via a
  //     nearby `mov x0,x22` (anonymous button) or an earlier `ldr x0,[x20,
  //     #0x5450]` (4th button), those loads are left untouched.
  // -------------------------------------------------------------------------

  // sub-mode-flag-set layout: button @ this+0x5440
  P_SYM(RACE_SETMENU_SYM, 0x138, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x138 button(0x5440) [flag-set layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x148, 0x1E270141, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x148 button(0x5440) [flag-set layout] "
    "bool arg=false"),

  // sub-mode-flag-set layout: button @ this+0x5448
  P_SYM(RACE_SETMENU_SYM, 0x200, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x200 button(0x5448) [flag-set layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x210, 0x1E270141, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x210 button(0x5448) [flag-set layout] "
    "bool arg=false"),

  // flag-clear layout: anonymous button (button vector only, no named field)
  P_SYM(RACE_SETMENU_SYM, 0x380, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x380 anonymous button [flag-clear layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x390, 0x1E270120, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x390 anonymous button [flag-clear layout] "
    "bool arg=false"),

  // flag-clear layout: button @ this+0x5440 (rebuilt at different coords)
  P_SYM(RACE_SETMENU_SYM, 0x444, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x444 button(0x5440) [flag-clear layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x454, 0x1E270141, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x454 button(0x5440) [flag-clear layout] "
    "bool arg=false"),

  // flag-clear layout: button @ this+0x5448 (rebuilt at different coords)
  P_SYM(RACE_SETMENU_SYM, 0x510, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x510 button(0x5448) [flag-clear layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x520, 0x1E270141, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x520 button(0x5448) [flag-clear layout] "
    "bool arg=false"),

  // flag-clear layout only: 4th button @ this+0x5450
  P_SYM(RACE_SETMENU_SYM, 0x618, 0xF9406508, 0xF940B908,
    "SceneSpecialRace::setMenu +0x618 button(0x5450) [flag-clear layout] "
    "setPosition->setVisible vtable slot"),
  P_SYM(RACE_SETMENU_SYM, 0x628, 0x1E232821, 0x2A1F03E1,
    "SceneSpecialRace::setMenu +0x628 button(0x5450) [flag-clear layout] "
    "bool arg=false"),

  // -------------------------------------------------------------------------
  // Fix 6: VirtualPad::openVPad -- green/yellow/red/blue color-button
  //         prompts (dance scene and other generic input/QTE scenes)
  //
  //     VirtualPad::openVPad() touches three fields in order:
  //
  //       this+0x360 -- the setupColorButtons Menu (dance/QTE buttons)
  //       this+0x368 -- the setupCursorColorButtons/setupPasscodeButtons
  //                     container Node (both builders store it at +0x368)
  //       this+0x370 -- nsMenu::nsInput::Manager*, un-paused by openVPad
  //
  //     Only hide the Menu at +0x360. The +0x368 container is functional:
  //     it holds the selectable letters for the LARA password and the
  //     cursor-based color controls. Hiding it makes these puzzles unusable
  //     even with a controller. Leave openVPad +0x4c's setVisible(true)
  //     and the input manager's setPause(false) unchanged.
  //
  //     Keep all setup calls: setupColorButtons initializes +0x360, which
  //     openVPad dereferences. closeVPad still hides both nodes and pauses
  //     the input manager when the puzzle closes.
  // -------------------------------------------------------------------------

  // openVPad +0x34: this+0x360 (setupColorButtons Menu) setVisible(true)
  // -> setVisible(false)
  P_SYM(VPAD_OPENVPAD_SYM, 0x34, 0x52800021, 0x2A1F03E1,
    "VirtualPad::openVPad +0x34 bool arg=false: color-button Menu "
    "(this+0x360) stays hidden"),

  // -------------------------------------------------------------------------
  // Fix 7: cSfcWork CONFIG_WORK constant -- Movement assumed-default
  //         RUN(0) -> WALK(1)
  //
  //     In-game text (FLD_CMES0_217/218) confirms a Settings > Movement
  //     option with two explicit states, Walk and Run, plus <BTN_DASH>
  //     always doing the opposite of whichever is selected. The runtime
  //     read of that field is FieldImpl::atel_isDash (0x58f398): it loads
  //     CONFIG_WORK.Movement from the emulated-WRAM mirror at
  //     [FieldImpl+72 (ChronoCanvas*), +0x14000, +564] and branches
  //     0=Run, 1=Walk, 2=(alt table, also Run-family) -- see the cmp/b.eq
  //     chain against #0x2/#0x1 right after the load. Nothing in that
  //     branch chain is touched by this fix; explicit 1 (Walk) and 2 (Run)
  //     selections resolve exactly as before.
  //
  //     The 0 case is what this fix changes -- but 0 is never *written* by
  //     any Settings-menu code path (MenuNodeConfig::changeRowValue only
  //     ever stores clamped 1/2-style indices back through the row's
  //     setter callback). It is the field's pre-user-input assumed value,
  //     coming from a 16-byte SIMD constant {1,1,0,0} at rodata 0x374b30
  //     that all three of cSfcWork::SetDefaultConfig (0x55c00c),
  //     ::InitNewGameData (0x55bca4), and ::InitNewGamePlusData (0x55c584)
  //     load via `ldr q0,[x8,#2864]` and store into CONFIG_WORK+52..67 --
  //     confirmed identical at all three sites via cross-reference. Word
  //     index 3 of that constant (byte offset 0x374b3c, CONFIG_WORK+64)
  //     lands on the exact same struct field atel_isDash reads (verified
  //     via the FieldImpl/ChronoCanvas offset chain and the GAME_DATA
  //     copy-constructor's memberwise-copy touching the same 564/568
  //     pair). Patching this single rodata word changes the pre-user-input
  //     default at all three call sites simultaneously; no code path,
  //     branch, or the Settings-menu UI logic is modified.
  //
  //     Word index 1 of the same constant (byte offset 0x374b34) is a
  //     verified non-zero neighbor (message-speed default, value 1) --
  //     used below as a same-value sanity check. apply_patch() only ever
  //     warns when cur != new_word AND old_word is non-zero, so an
  //     old==new==1 entry stays silent for as long as the constant blob
  //     hasn't shifted, and flags a MISMATCH (without writing anything) if
  //     a future rebuild moves this rodata layout -- a real verification
  //     gate, unlike old=0 on the actual target word below, which P_RAW's
  //     "don't verify" sentinel can't distinguish from an unchecked cave
  //     slot. This word is genuinely, verifiedly zero in this build; 0 is
  //     the correct old-value to write here, just not one this framework
  //     can double-check on its own.
  // -------------------------------------------------------------------------

  // Sanity check only (no-op while layout is unchanged): rodata 0x374b34,
  // neighboring word in the same CONFIG_WORK-defaults constant, expected
  // to remain 1. If a rebuild ever shifts this blob, this fires a
  // MISMATCH in the debug log for the entry below instead of silently
  // writing to the wrong address.
  P_RAW(0x374b34, 0x00000001, 0x00000001,
    "sanity check: cSfcWork CONFIG_WORK-defaults constant neighbor word "
    "unchanged (verifies rodata layout before the Movement-default patch)"),

  // The actual fix: rodata 0x374b3c, CONFIG_WORK.Movement assumed-default
  // word, RUN(0) -> WALK(1). old=0 is a verified real value here (see
  // derivation above), not an unchecked cave slot.
  P_RAW(0x374b3c, 0x00000000, 0x00000001,
    "cSfcWork CONFIG_WORK.Movement assumed-default RUN(0)->WALK(1); "
    "explicit user Walk/Run choice in Settings still respected by "
    "FieldImpl::atel_isDash"),
};

// ---------------------------------------------------------------------------
// 4.  controller_glyphs
//
//     In-message <BTN_*> tags (CONF/DASH/MENU/WARP/L/R) are substituted at
//     render time from one of two .bss glyph-string tables, chosen by the
//     message-text builder at 0x5fea40:
//
//       and  x8, w20, #3                  ; w20 = ChronoCanvas[0x10b2c] (A/B-swap cfg)
//       umaddl x8, w8, #0x90, 0xbf3798    ; x8 = CONTROLLER table + (swap&3)*0x90
//       add  x9, .., #0x9d8               ; x9 = KEYBOARD table 0xbf39d8
//       tst  w0, #1                       ; w0 = "last input was a key" (pad? bit)
//       csel x20, x8, x9, ne              ; pick keyboard when a key was last seen
//
//     With native_controller on this already resolves to the controller table,
//     but forcing the select guarantees Switch-button glyphs even on the very
//     first frame or in keyboard-compat mode. csel -> mov x20,x8 (controller).
//     The (swap&3) index is preserved, so the A/B-swap setting is still honoured.
// ---------------------------------------------------------------------------
static const PatchEntry g_glyph_patches[] = {
  P_RAW(0x5fea50, 0x9a891114, 0xaa0803f4,
    "MsgText <BTN_*> tags: force controller glyph table (csel x20,x8,x9,ne -> mov x20,x8)"),

  // ---------------------------------------------------------------------
  // <BTN_*> glyphs: bracketed letters -> plain "[A]"-style ASCII glyphs
  //
  // The builder at 0x5fce6c assembles each glyph string inline in registers
  // (movz/movk) then stores it into every swap-variant slot of TWO parallel
  // tables: a KEY table at 0xbf3708 (the <BTN_*> search strings) and a VALUE
  // table at 0xbf3798 (the glyph each key is replaced with). The dialogue
  // text builder runs std::string::replace(key -> value) for all six rows.
  //
  // Originally each VALUE glyph was the 7-byte UTF-8 string
  //   U+3010 <letter> U+3011   ( e.g. e3 80 90 41 e3 80 91 = bracketed 'A' )
  // built as two overlapping 4-byte halves in a _lo/_hi register pair. We
  // replace each with the plain 3-byte ASCII string "[<letter>]" (e.g.
  // "[A]" = 5b 41 5d), so the button prompt renders as ordinary text in
  // whichever font is already drawing the surrounding dialogue -- no
  // Private-Use codepoints and no separate shared-font lookup required.
  //
  // CRITICAL: the per-glyph SSO size byte must be set WITHOUT shrinking the
  // search keys. The builder originally sourced BOTH the glyph size and the
  // key size from w20, so shrinking w20 also truncated <BTN_R>/<BTN_L> to
  // <BT, leaving 'N_R>'/'N_L>' after the icon. We therefore leave w20 at its
  // original 0x0e (keys stay length 7) and instead route every glyph-VALUE
  // size byte through w16 (set to 0x06 = length 3), which the L/R glyphs
  // already used. Per glyph: set _lo = '5b <letter> 5d 00', zero _hi (NUL
  // pad) -- the ASCII form is the same 3-byte length as the old PUA glyph,
  // so the size-byte plumbing below is unchanged.
  // ---------------------------------------------------------------------
  // <BTN_A> glyph -> "[A]" (5b 41 5d)
  P_RAW(0x5fce84, 0x52901c68, 0x52882b68,
    "<BTN_A> glyph lo: movz w8,#0x415b (ASCII 5b 41 = \"[A\")"),
  P_RAW(0x5fceac, 0x72a83208, 0x72a00ba8,
    "<BTN_A> glyph lo: movk w8,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fce88, 0x529c682e, 0x5280000e,
    "<BTN_A> glyph hi: movz w14,#0 (NUL pad)"),
  P_RAW(0x5fceb0, 0x72b2300e, 0x72a0000e,
    "<BTN_A> glyph hi: movk w14,#0,lsl16"),
  // <BTN_B> glyph -> "[B]" (5b 42 5d)
  P_RAW(0x5fce8c, 0x52901c75, 0x52884b75,
    "<BTN_B> glyph lo: movz w21,#0x425b (ASCII 5b 42 = \"[B\")"),
  P_RAW(0x5fceb4, 0x72a85215, 0x72a00bb5,
    "<BTN_B> glyph lo: movk w21,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fce90, 0x529c6856, 0x52800016,
    "<BTN_B> glyph hi: movz w22,#0 (NUL pad)"),
  P_RAW(0x5fceb8, 0x72b23016, 0x72a00016,
    "<BTN_B> glyph hi: movk w22,#0,lsl16"),
  // <BTN_X> glyph -> "[X]" (5b 58 5d)
  P_RAW(0x5fce94, 0x52901c6a, 0x528b0b6a,
    "<BTN_X> glyph lo: movz w10,#0x585b (ASCII 5b 58 = \"[X\")"),
  P_RAW(0x5fcebc, 0x72ab120a, 0x72a00baa,
    "<BTN_X> glyph lo: movk w10,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fce98, 0x529c6b0f, 0x5280000f,
    "<BTN_X> glyph hi: movz w15,#0 (NUL pad)"),
  P_RAW(0x5fcec0, 0x72b2300f, 0x72a0000f,
    "<BTN_X> glyph hi: movk w15,#0,lsl16"),
  // <BTN_Y> glyph -> "[Y]" (5b 59 5d)
  P_RAW(0x5fce9c, 0x52901c6b, 0x528b2b6b,
    "<BTN_Y> glyph lo: movz w11,#0x595b (ASCII 5b 59 = \"[Y\")"),
  P_RAW(0x5fcec4, 0x72ab320b, 0x72a00bab,
    "<BTN_Y> glyph lo: movk w11,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fcea0, 0x529c6b31, 0x52800011,
    "<BTN_Y> glyph hi: movz w17,#0 (NUL pad)"),
  P_RAW(0x5fcec8, 0x72b23011, 0x72a00011,
    "<BTN_Y> glyph hi: movk w17,#0,lsl16"),
  // <BTN_R> glyph -> "[R]" (5b 52 5d)   |   <BTN_L> glyph -> "[L]" (5b 4c 5d)
  P_RAW(0x5fce6c, 0xd2901c6c, 0xd28a4b6c,
    "<BTN_R> glyph: movz x12,#0x525b (ASCII 5b 52 = \"[R\")"),
  P_RAW(0x5fce7c, 0xf2aa520c, 0xf2a00bac,
    "<BTN_R> glyph: movk x12,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fcea4, 0xf2dc684c, 0xf2c0000c,
    "<BTN_R> glyph: movk x12,#0,lsl32 (NUL pad)"),
  P_RAW(0x5fced0, 0xf2f2300c, 0xf2e0000c,
    "<BTN_R> glyph: movk x12,#0,lsl48 (NUL pad)"),
  P_RAW(0x5fce70, 0xd2901c6d, 0xd2898b6d,
    "<BTN_L> glyph: movz x13,#0x4c5b (ASCII 5b 4c = \"[L\")"),
  P_RAW(0x5fce80, 0xf2a9920d, 0xf2a00bad,
    "<BTN_L> glyph: movk x13,#0x005d,lsl16 (ASCII 5d 00 = \"]\\0\")"),
  P_RAW(0x5fcea8, 0xf2dc684d, 0xf2c0000d,
    "<BTN_L> glyph: movk x13,#0,lsl32 (NUL pad)"),
  P_RAW(0x5fced4, 0xf2f2300d, 0xf2e0000d,
    "<BTN_L> glyph: movk x13,#0,lsl48 (NUL pad)"),

  // --- key/glyph size-byte decoupling (the fix for the 'N_R>' leak) ---
  // w20 is left UNTOUCHED at 0x0e so all six <BTN_*> search keys keep their
  // full length. w16 becomes 0x06 and is used as the size byte for every
  // glyph VALUE row (A/B/X/Y newly routed to it; L/R already used it).
  P_RAW(0x5fcecc, 0x52800210, 0x528000d0,
    "glyph size reg: mov w16,#0x10 -> #0x06 (SSO len 3, shared by all 6 glyph values)"),
  // Route the 15 A/B/X/Y glyph-value size stores from w20 -> w16 so they
  // get length 3 while the keys (still w20) keep length 7.
  P_RAW(0x5fcedc, 0x39000134, 0x39000130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcef4, 0x39006134, 0x39006130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x18] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf04, 0x3900c134, 0x3900c130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x30] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf14, 0x39012134, 0x39012130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x48] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf48, 0x3902a134, 0x3902a130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0xa8] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf58, 0x39030134, 0x39030130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0xc0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf68, 0x39036134, 0x39036130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0xd8] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf8c, 0x39048134, 0x39048130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x120] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf98, 0x3904e134, 0x3904e130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x138] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfa8, 0x39054134, 0x39054130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x150] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfb8, 0x3905a134, 0x3905a130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x168] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfd8, 0x3906c134, 0x3906c130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1b0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfe4, 0x39072134, 0x39072130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1c8] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcff4, 0x39078134, 0x39078130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1e0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fd000, 0x3907e134, 0x3907e130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1f8] (use size-3 reg, not key-size reg)"),
};

// ---------------------------------------------------------------------------
// 5.  diagonal_movement
//
//     Same bug class CTExt fixes on PC (FieldImpl::UserScrollDiagonal hook,
//     ctext.hooks.field_impl.ixx: "Fixes the diagonal movement stutter bug by
//     reverting to the original behaviour"). FieldImpl::UserScroll (0x5a1380)
//     is one shared switch over the 8-direction input mask (w8 = case index):
//
//       cardinal cases (w8=0,1,3,7 = up/down/left/right): raw per-axis delta
//         (+-0x10/0x20 walk/run) is stored to [x8,#0x90/#0x94] AND committed
//         straight into the X/Y target accumulators at [x8,#0x98]/[x8,#0xa4]
//         every frame -- smooth, constant per-frame step.
//
//       diagonal cases (w8=4,5,8,9 = the 4 diagonals): the SAME raw delta is
//         stored to [x8,#0x90]/[x8,#0x94] (so cardinal-style consumers see
//         the right magnitude), but the X/Y TARGET accumulators instead go
//         through an extra float path: divide the raw delta by 0x3fb1eb85
//         (1.39, a rough "diagonal speed compensation" constant -- notably
//         NOT sqrt(2)=1.41421...), truncate to int (fcvtzs = round toward
//         zero) and add. Truncating every single frame instead of carrying
//         the fractional remainder means the target consistently advances
//         slower (~23/frame instead of 32/frame, etc.) than the raw/cardinal
//         rate -- a per-frame divergence between what's stored as "raw" and
//         what the camera/position actually commits to. That's the stutter.
//
//     Fix: make diagonal commit the raw delta directly, exactly like
//     cardinal already does (target += raw, no normalize/truncate), instead
//     of porting the float math byte-for-byte. Each of the 4 diagonal blocks
//     ends with a "cbnz w12, <go down the float path>" immediately after
//     storing the raw delta to [x8,#0x90]/[x8,#0x94] -- redirect each of
//     those 4 branches (unconditionally) into a small shared cave that does
//     the same raw-int accumulate the cardinal blocks use, then jumps to the
//     function's existing epilogue. Verified: redirecting these 4 sites
//     touches only diagonal-block code; the cardinal blocks' own commit site
//     (0x5a16bc) and the function epilogue (0x5a16c8) are untouched and are
//     also where this cave's last instruction lands.
//
//     Net gameplay effect: diagonal movement speed equals raw axis speed
//     (matches cardinal exactly) instead of running ~28% slower with visible
//     truncation-stutter -- the same direction CTExt's PC fix takes (drop
//     the lossy per-frame normalize/truncate, accumulate raw deltas).
// ---------------------------------------------------------------------------
static const PatchEntry g_diagonal_patches[] = {
  // Redirect each diagonal block's post-store branch into the shared cave below.
  // Original instruction at each site is the first half of a float divide-by-1.39
  // "diagonal speed normalization" (cbnz into the float path); replacing it with an
  // unconditional branch skips that path entirely for all 4 diagonal blocks.
  P_RAW(0x5a14e4, 0x35000a0c, 0x1400003a,
    "UserScroll diagonal block w8=4 (down-right/up-right family): skip float normalize, use raw-int accumulate"),
  P_RAW(0x5a151c, 0x35000a0c, 0x1400002c,
    "UserScroll diagonal block w8=9: skip float normalize, use raw-int accumulate"),
  P_RAW(0x5a1570, 0x350003ac, 0x14000017,
    "UserScroll diagonal block w8=5: skip float normalize, use raw-int accumulate"),
  P_RAW(0x5a15c8, 0x3500018c, 0x14000001,
    "UserScroll diagonal block w8=8: skip float normalize, use raw-int accumulate"),

  // Cave @ 0x5a15cc (40 bytes of now-dead code after the 4 redirects above;
  // verified empty/unreachable -- nothing else in the function branches here).
  P_CAVE(0x5a15cc, 0x29522d0a, "cave: ldp w10,w11,[x8,#0x90]   ; w10,w11 = raw per-axis step just stored (same magnitude cardinal uses)"),
  P_CAVE(0x5a15d0, 0xb940990c, "cave: ldr w12,[x8,#0x98]       ; w12 = current X target"),
  P_CAVE(0x5a15d4, 0xb0a018c, "cave: add w12,w12,w10          ; X target += raw X step (no normalization, no truncation loss)"),
  P_CAVE(0x5a15d8, 0xb900990c, "cave: str w12,[x8,#0x98]"),
  P_CAVE(0x5a15dc, 0xb900a10c, "cave: str w12,[x8,#0xa0]       ; commit X (matches cardinal's commit site)"),
  P_CAVE(0x5a15e0, 0xb940a509, "cave: ldr w9,[x8,#0xa4]        ; w9 = current Y target"),
  P_CAVE(0x5a15e4, 0xb0b0129, "cave: add w9,w9,w11            ; Y target += raw Y step"),
  P_CAVE(0x5a15e8, 0xb900a509, "cave: str w9,[x8,#0xa4]"),
  P_CAVE(0x5a15ec, 0xb900ad09, "cave: str w9,[x8,#0xac]        ; commit Y (matches cardinal's commit site)"),
  P_CAVE(0x5a15f0, 0x14000036, "cave: b 0x5a16c8 (UserScroll epilogue, restores x19/x29/x30 and returns)"),
};

// ---------------------------------------------------------------------------
// 6.  fixed_timestep
//
//     Root cause of the "everything shimmers/scoots while moving, clean when
//     stationary" artifact -- confirmed by frametime.log: a locked ~60fps
//     (avg 16.72ms, ~0 dropped frames) but with heavy per-frame jitter (a large
//     share of frames land 17-20ms while others land short). cocos2d advances
//     ALL motion by Director::_deltaTime (Director+0xe8), which Director::
//     drawScene recomputes every frame as the raw wall-clock gap since the
//     previous frame:
//
//         _deltaTime = max( (now - _lastUpdate) seconds , 0 )   // no smoothing
//
//     Vsync pins presentation to a clean 60Hz, but that measured gap still
//     wobbles, so constant-speed motion advances by an uneven number of pixels
//     each displayed frame. The eye integrates the unevenness as a directional
//     smear -- worse the faster you move, a faint "flag in the wind" wave when
//     slow, gone at rest -- and it hits everything on that clock: field scroll,
//     free sprites, and sliding menu text alike. Notably it is NOT a scaling or
//     sampling artifact (identical with bilinear on or off) and NOT dropped
//     frames (over34ms ~= 0), which is what ruled every earlier theory out.
//
//     CT is frame-based at heart (the SNES ran a rock-steady 60Hz); its motion
//     logic wants a constant step. Fix: force _deltaTime to a fixed 1/60s every
//     frame instead of the jittery measured value, so every frame advances by
//     exactly the same amount. Patched at BOTH writers of _deltaTime:
//     Director::drawScene (the live per-frame path; dt is inlined there) and the
//     sibling Director::calculateDeltaTime. At each site the dead measured-dt
//     computation block is overwritten IN PLACE with "w8 = 1/60 bits; s0 =
//     (float)w8; branch to the existing _deltaTime store". The _nextDeltaTimeZero
//     reset path (dt forced to 0 for one frame after a pause/scene load) and the
//     _lastUpdate write are both left untouched, so only the normal-frame delta
//     changes. 1/60 = 0x3c888889; it is too small to encode as fmov #imm, hence
//     the movz/movk + fmov s0,w8.
//
//     Trade-off: if the game ever SUSTAINS below 60fps (rare here), a fixed step
//     makes motion run real-time-slow for those frames rather than skipping --
//     for a single-player RPG that is the right call and far less visible than
//     the judder. Disable this flag to revert to the original measured dt.
// ---------------------------------------------------------------------------
static const PatchEntry g_fixed_timestep_patches[] = {
  // Director::drawScene @0x8d85a0 -- live per-frame dt store. Overwrite the
  // else-branch (measured-dt math) with: w8=0x3c888889; s0=(float)w8; b <store>.
  P_RAW(0x8d85dc, 0xf940ba68, 0x52911128, "drawScene: movz w8,#0x8889 (lo half of 1/60)"),
  P_RAW(0x8d85e0, 0xcb080008, 0x72a79108, "drawScene: movk w8,#0x3c88,lsl#16 -> w8 = 1/60 bits"),
  P_RAW(0x8d85e4, 0x9b557d08, 0x1e270100, "drawScene: fmov s0,w8 (dt = 1/60)"),
  P_RAW(0x8d85e8, 0x9347fd09, 0x14000009, "drawScene: b 0x8d860c (fall into the _deltaTime store)"),

  // Director::calculateDeltaTime @0x8d8888 -- sibling copy, patched identically.
  P_RAW(0x8d88b0, 0xd29ef9e9, 0x52911128, "calcDeltaTime: movz w8,#0x8889"),
  P_RAW(0x8d88b4, 0xf940ba68, 0x72a79108, "calcDeltaTime: movk w8,#0x3c88,lsl#16"),
  P_RAW(0x8d88b8, 0xf2bc6a69, 0x1e270100, "calcDeltaTime: fmov s0,w8"),
  P_RAW(0x8d88bc, 0xf2d374a9, 0x1400000d, "calcDeltaTime: b 0x8d88f0 (fall into the _deltaTime store)"),
};

// ---------------------------------------------------------------------------
// 7.  ui_scale_fix
//
//     ROOT CAUSE of the motion shimmer / "scooting" / thinning-eyes artifact
//     (confirmed via scale_diag.log, not guessed): AppDelegate::
//     applicationDidFinishLaunching hardcodes the cocos2d-x design resolution
//     to 568x320 -- leftover iPhone-5 boilerplate (aspect 568/320 = 1.775),
//     completely unrelated to the Switch's panel. cocos2d::GLView::
//     updateDesignResolutionSize computes mScaleX = frameWidth/designWidth
//     and mScaleY = frameHeight/designHeight; every Switch panel is exact
//     16:9, so this always lands on 1280/568 = 720/320 = 2.253521... --
//     never an integer, on every resolution, handheld or docked. That
//     fractional scale is what every layer (field render-targets, sprites,
//     and UI/menu text alike) gets stretched by, and GL_NEAREST sampling of
//     a non-integer, per-frame-shifting scroll position is exactly what
//     reads as scoot/shimmer in motion and clean at rest.
//
//     This supersedes the earlier integer_scale_fix attempt (rounding
//     mScaleX/mScaleY after the fact, at 0x8b76b4) -- that only changed the
//     zoom level, because the *input* to the division was still the wrong
//     design resolution. Fixing the actual constant is the correct place:
//     640x360 is exact 16:9, so it divides every Switch resolution cleanly
//     -- 1280x720 -> 2.0x, 1920x1080 -> 3.0x, 2560x1440 -> 4.0x -- with no
//     rounding, no cave, no downstream patch needed at all.
//
//     Patches the two float-literal immediates in the small unnamed static
//     initializer that populates the Size table AppDelegate reads from
//     (function at 0x641bf4, no exported symbol): 568.0 -> 640.0 and
//     320.0 -> 360.0. Both original and replacement values happen to encode
//     with zero low-16-bits, so each is a single "mov w_,#imm" instruction
//     (movz, no movk needed) -- a pure immediate swap, no branches added.
//
//     Trade-off: at the new design resolution the game shows ~12% more of
//     the world per screen edge-to-edge (568/320 -> 640/360 is a small
//     zoom-out), since more world now maps to the same screen. Sprite/UI
//     pixel density is unaffected -- this is a scale-alignment fix, not a
//     resize -- only the visible margin changes slightly.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Design resolution fix, table-wide + config-driven.
//
// The game does NOT have one design resolution: a static initializer at
// 0x641c00.. builds an aspect-bucketed TABLE of candidate design Sizes --
// (568x320) 16:9, (480x360) 4:3, (568x340) 16:10, (1680x720) ultrawide --
// and a runtime picker selects from it on every surface event (boot, dock,
// undock). Patching only the 16:9 entry (all earlier builds) left the picker
// free to apply other entries after resize events, producing the observed
// boot-vs-redock inconsistency and, in one state, width from one entry with
// height from another (480x360 -> scaleX=4 / scaleY=3 at 1080p: uniform
// widths, broken heights -- the "rectangles").
//
// Fix: stamp EVERY entry in the table with the same (w,h), so the picker's
// choice is irrelevant. Values are config-driven per mode (see config.h) so
// the scale/framing trade-off can be A/B'd from config.ini without rebuilds.
// The (568x340) entry's width register reuses the 16:9 entry's (s8), so only
// its height instruction exists to patch -- width follows automatically.
// ---------------------------------------------------------------------------

// Encode: movz w<rd>, #(top 16 bits of float v), lsl #16
static uint32_t movz_topf(int rd, float v) {
  union { float f; uint32_t u; } x; x.f = v;
  return 0x52A00000u | ((x.u >> 16) << 5) | (uint32_t)rd;
}

static void apply_ui_scale_fix(so_module *mod, float w, float h) {
  static const struct { uint32_t va; int rd; float oldv; int is_h; } sites[] = {
    { 0x641c34, 8,  568.0f, 0 },  // 16:9 entry, width
    { 0x641c38, 9,  320.0f, 1 },  // 16:9 entry, height
    { 0x641c4c, 8,  480.0f, 0 },  // 4:3 entry, width
    { 0x641c50, 9,  360.0f, 1 },  // 4:3 entry, height
    { 0x641c68, 8,  340.0f, 1 },  // 16:10 entry, height (width = 16:9 width via s8)
    { 0x641c80, 8, 1680.0f, 0 },  // ultrawide entry, width
    { 0x641c84, 9,  720.0f, 1 },  // ultrawide entry, height
  };
  PatchEntry e[7];
  for (int i = 0; i < 7; i++) {
    e[i].sym_name  = NULL;
    e[i].func_off  = 0;
    e[i].raw_vaddr = sites[i].va;
    e[i].old_word  = movz_topf(sites[i].rd, sites[i].oldv);
    e[i].new_word  = movz_topf(sites[i].rd, sites[i].is_h ? h : w);
    e[i].desc      = "design resolution table entry (unified per-mode value)";
  }
  apply_patches(mod, e, 7);
}

// ---------------------------------------------------------------------------
// 8.  game_area_width_fix
//
//     ROOT CAUSE of the non-square art pixels (squares wider than tall by
//     ~1.127 = 640/568), measured three times from panel crops and invariant
//     across handheld/docked and across every screen_width/height combination
//     -- which is why hand-tuning the surface size could approach but never
//     reach square: the error is upstream of both scaling stages.
//
//     AppDelegate::applicationDidFinishLaunching ends by constructing the
//     global cocos2d::Rect  ctr::gameArea  (exported symbol _ZN3ctr8gameAreaE)
//     at 0x641a54..0x641ac0. Its HEIGHT is adaptive (visibleSize.height
//     based -- the same adaptive style as the field code's visH-320 and
//     visH*192/320 math), but its WIDTH is the hardcoded float 568.0:
//
//         gameArea.width  = 568 - 2*visibleOrigin.x        (568 literal)
//         gameArea.height = visibleSize.height - y_offset  (adaptive)
//
//     568 is the iPhone-5 design width the port was authored against. With
//     the stock 568x320 design this rect exactly equals the visible area, so
//     the mismatch is invisible on Android. With ui_scale_fix
//     widening the design to 640x360, the game keeps laying the world out
//     568 wide and fills the 640-wide screen with it: a 640/568 = 1.1268
//     horizontal-only stretch. Vertical stays clean because the height side
//     was adaptive all along -- matching the measurement exactly (vertical
//     uniform, horizontal 6,6,5 / 4,4,4,5 patterns, ratio 1.123-1.133).
//
//     Fix: make the width adaptive the same way the height already is.
//     visibleRect is on the stack at [sp+0x10..0x1f] at this point (sret of
//     the getVisibleRect vtable call at 0x641a64; width at [sp+0x18] --
//     the code itself loads it from there at 0x641a6c):
//
//       0x641a94  mov  w8,#0x440e0000 (568.0f) -> ldr s6,[sp,#0x18] (visW)
//       0x641a98  fmov s6,w8                   -> nop
//       0x641ab0  fadd s0,s3,s0 (x=2*ox+m-44)  -> fmov s0,s3 (x=origin.x)
//
//     giving gameArea = (origin.x, y, visW - 2*origin.x, h). Provably a
//     NO-OP at the stock 568-wide design (origin.x=0: x 0->0, w 568->568),
//     so it only takes effect exactly where the bug exists. w8 is dead
//     between 0x641a98 and its next write at 0x641ac4, and s6 is unread
//     between the new load and its consumption at 0x641aa8 (verified in
//     disassembly). Encodings verified with aarch64-linux-gnu-as.
//
//     STATUS (on-device testing): patch applies and boots clean, but the
//     field-map stretch was not visibly cured by it -- gameArea's 20 reader
//     sites are UI/menu/world-map code, and the field pipeline appears to
//     carry its OWN copy of the 568-era horizontal constants (hardcoded
//     44/480 margin math in FieldMap::setScrollLimit at 0x570cec, vs its
//     adaptive visH-based vertical math). Panel-crop measurement pending to
//     confirm; if confirmed, the follow-up patch targets those field
//     constants the same way. Keep this fix on regardless: it is the
//     correct generalization for every gameArea consumer.
// ---------------------------------------------------------------------------
static const PatchEntry g_gamearea_patches[] = {
  P_RAW(0x641a94, 0x52a881c8, 0xbd401be6,
        "gameArea: width literal 568.0 -> ldr s6,[sp,#0x18] (visible width)"),
  P_RAW(0x641a98, 0x1e270106, 0xd503201f,
        "gameArea: fmov s6,w8 -> nop (s6 now loaded directly)"),
  P_RAW(0x641ab0, 0x1e202860, 0x1e204060,
        "gameArea: x = origin.x (fadd s0,s3,s0 -> fmov s0,s3)"),
};

// ---------------------------------------------------------------------------
// 9.  field_zoom_fix
//
//     Full-frame measurement (1280x720 screenshot, autocorrelation over the
//     whole panel) pinned the field's art-pixel size to exactly 3.75 x
//     3.3333 panel px -- ratio 9/8 -- with a visible world of 341.33 x 216
//     art px. Those decode to the game's view-sizing formulas verbatim:
//     216 = visibleHeight*192/320 (FieldMap::init -> [this+0x34c]) and
//     341.33 = visibleWidth*256/480 (setScrollLimit). The port sizes its
//     field view at 256 art px per 480 design units horizontally but 192
//     per 320 vertically: densities 8/15 vs 3/5, unequal by exactly 9/8.
//     The same formulas produce the same 9/8 on the original iPhone-5
//     layout -- i.e. this is Square Enix's deliberate approximation of the
//     SNES/CRT pixel aspect (true CRT: 8/7), not an accident. It is also
//     why no design/surface/GL configuration could ever remove it.
//
//     This patch sets both densities to exactly 1/2 (numerators only:
//     192 -> 160 = 320/2, 256 -> 240 = 480/2; the 320/480/-320/44 authored
//     constants are left alone so every margin/limit expression stays
//     dimensionally consistent). Result with the 640x360 design: the view
//     becomes 320x180 art px -- 16:9 in art terms -- so art pixels are
//     4x4 panel px in handheld and 6x6 docked: square AND integer in both
//     modes, eliminating both the aspect stretch and the uneven
//     tight/loose pixel-width pattern (the motion shimmer). The field
//     camera is ~7%% tighter horizontally and ~17%% vertically than the
//     CRT-style view; menus/UI do not use these formulas and are
//     unaffected. The two vertical-density sites (init, setScrollLimit)
//     and the single horizontal site (setScrollLimit) are patched
//     together so view size and scroll limits move coherently. Encodings
//     verified with aarch64-linux-gnu-as.
//
//     CORRECTION: a fourth site at 0x608bf0, previously patched here as a
//     third "vertical-density"/"camera-follow" site, was mis-scoped. It
//     resolves (via .dynsym) to WorldMap::update, not anything in
//     FieldMap -- it's overworld tile-animation-timing math that
//     coincidentally starts from the same 192.0f constant. It has been
//     silently overwritten (192.0 -> 160.0, and later -> 320.0/zoom) by
//     every build since the original fixed-value field_zoom_fix,
//     never validated against the overworld screen specifically. Dropped
//     from this patch set entirely; it's no longer touched, so it stays
//     at its original stock 192.0f in all configurations.
//
//     CORRECTION (superseding the previous handoff note below): the rodata
//     pair at 0x360b08 is NOT the blit-scale source. Symbol resolution
//     (.dynsym) shows its two readers are FieldMap::onTouchMoved and
//     FieldMap::onTouchEnded -- touch-drag delta / tap-to-tile hit-testing,
//     mobile-only input code, never on the render path. That is exactly why
//     patching it produced no display effect. It's left patched below
//     anyway (harmless) purely so touch-drag math stays dimensionally
//     consistent with the density patches above, on the off chance the
//     touch screen is used in handheld mode.
//
//     The actual blit-scale source: FieldMap::makeField (0x57520c-0x576668)
//     builds a child Node -- internally named "fieldmap" via an inlined
//     8-byte string constant a few instructions earlier -- adds it as a
//     child, stashes the pointer at field+0x3b8, and immediately calls
//     Node::setScale(x,y) (vtable slot 0x90) on it with two HARD-CODED
//     immediates: s0 = 1.875f via a single `fmov` (exactly representable in
//     the 8-bit float-immediate encoding), s1 = 1.66667f assembled into w9
//     via `mov`+`movk` (0x3FD55555, NOT exactly representable as an fmov
//     immediate) then moved with `fmov s1, w9`. This is a third, previously
//     unknown occurrence of the (1.875, 1.66667) ratio -- distinct from
//     both the density formulas and the rodata pair above -- and it's
//     never loaded from memory, which is why full-frame measurement showed
//     H locked at 3.75 px/art through every prior patch: 1.875 * 2.0 =
//     3.75 and 1.66667 * 2.0 = 3.33334 match the measured art-pixel size
//     exactly, where the outer 2.0/3.0 is the handheld/docked content-scale
//     factor applied downstream of this node. Setting both to 2.0 here
//     composes to the same 4x4 handheld / 6x6 docked target as the density
//     patches above, on the actual render path this time.
//
//     Instruction count and addresses are unchanged, so nothing downstream
//     shifts. Encodings verified with keystone (cross-checked against the
//     original fmov #1.875 bytes) and re-disassembled after patching to
//     confirm a clean result with no corruption of the neighboring
//     `ldr`/`mov`/`blr` instructions.
//
//     CONFIRMED ON HARDWARE at zoom=2.0: autocorrelation measured exactly
//     4.0000x4.0000 handheld / 6.0000x6.0000 docked.
//
// ---------------------------------------------------------------------------
// field_zoom -- the makeField setScale value is config-driven (see config.h)
// instead of a fixed 2.0, so you can trade a bit of "zoomed in" for more
// visible map without giving up square pixels. All 4 instruction slots are
// reused as a single general "load this float into w9, fmov both s0 and s1
// from it" sequence, so any float works -- not just the handful directly
// representable as an ARM64 fmov-immediate:
//
//   movz w9, #lo16(bits(zoom))
//   movk w9, #hi16(bits(zoom)), lsl #16
//   fmov s0, w9
//   fmov s1, w9
//
// s0/s1 always get the SAME value, so the result is always square/uniform
// (no X/Y stretch) for ANY zoom you pick -- that part of "uniform scaling"
// holds unconditionally.
//
// The OTHER part -- exact integer art-pixel size with no motion shimmer --
// depends on the outer display scale too, which ui_scale_fix pins
// at exactly 2x in handheld and 3x in docked (screen / 640 design width).
// Final art-px-per-screen-px = zoom * 2 (handheld) and zoom * 3 (docked).
// Since gcd(2,3) = 1, the only values that make BOTH of those come out as
// whole numbers are whole-number zoom values themselves:
//   zoom=1 -> 2x handheld / 3x docked   (zoomed OUT: more map, smaller art)
//   zoom=2 -> 4x handheld / 6x docked   (default, current shipped feel)
//   zoom=3 -> 6x handheld / 9x docked   (zoomed IN)
// Non-integer zoom (e.g. 1.5) still renders square, undistorted pixels --
// it just isn't guaranteed to land on a whole screen-pixel boundary in both
// modes, which can bring back the sub-pixel scroll shimmer in whichever
// mode(s) it doesn't divide evenly for. If you only ever play in one mode,
// a value that's merely a multiple of 1/2 (handheld) or 1/3 (docked) will
// stay perfectly crisp in that mode alone.
// ---------------------------------------------------------------------------
static const PatchEntry g_field_zoom_fix_patches[] = {
  // The 3 view-density sites (FieldMap::init view-height, setScrollLimit
  // width+height) used to be hardcoded here (192->160, 256->240 -- the
  // fixed 320x180-art-px view that only matches zoom=2.0). A 4th site
  // (WorldMap::update, mislabeled "camera-follow") was patched here too in
  // earlier builds; it's not part of FieldMap and has been removed -- see
  // apply_field_view_zoom's header comment for the full story.
  // They're now zoom-dependent -- see apply_field_view_zoom() below, called
  // alongside apply_field_zoom() -- so the view (how much map is visible)
  // always scales inversely with zoom (the blit density), keeping the
  // rendered content pinned to exactly fill the 640x360 design canvas at
  // ANY zoom setting. Fixing them here would fight that.
  //
  // Touch-drag / hit-test converters only -- confirmed no display effect.
  // Kept for touch-drag consistency; safe no-op for controller-only play.
  P_RAW(0x360b08, 0x3ff00000, 0x40000000, "onTouchMoved/Ended: rodata view scale X: 1.875f -> 2.0f"),
  P_RAW(0x360b0c, 0x3fd55555, 0x40000000, "onTouchMoved/Ended: rodata view scale Y: 1.66667f -> 2.0f"),
  // The makeField "fieldmap" node setScale(x,y) call is patched separately,
  // below, via apply_field_zoom() -- its value is config.field_zoom, not a
  // fixed constant.
};

// Encode: movz w<rd>, #imm16
static uint32_t movz_w(int rd, uint16_t imm16) {
  return 0x52800000u | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}
// Encode: movk w<rd>, #imm16, lsl #16
static uint32_t movk_w_hi(int rd, uint16_t imm16) {
  return 0x72A00000u | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}
// Encode: fmov s<sd>, w<wn>  (GP register -> single-precision FP register)
static uint32_t fmov_s_from_w(int sd, int wn) {
  return 0x1E270000u | ((uint32_t)wn << 5) | (uint32_t)sd;
}
// Encode: add x<rd>, x<rn>, #imm12  (64-bit GP form -- add_w_imm above is 32-bit)
static uint32_t add_x_imm(int rd, int rn, uint32_t imm12) {
  return 0x91000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
// Encode: stp s<rt>, s<rt2>, [x<rn>, #imm]  (32-bit SIMD&FP pair, signed
// offset, no writeback -- imm must be a multiple of 4, passed pre-divided).
static uint32_t stp_s_imm(int rt, int rt2, int rn, int imm_over_4) {
  return 0x2D000000u | (((uint32_t)imm_over_4 & 0x7Fu) << 15) |
         ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
// (An `add w<rd>, w<rn>, #imm12` encoder was sketched here for a possible
// setScrollLimit Y-max small-room compensation term (candidate site
// 0x570e48). That site sits inside setScrollLimit's tile-window integer
// math, which reuses registers across both axes heavily enough that it
// couldn't be pinned down with confidence in the time available -- unlike
// the sites below, which were each verified end-to-end against the actual
// disassembly. Left out rather than guessed at; see apply_field_camera_-
// centering's header comment for what this affects and how to pick it
// back up if you want to chase it.)

// ---------------------------------------------------------------------------
// apply_field_view_zoom -- keeps FieldMap's view-size and camera-limit
// constants in lock-step with field_zoom, so the fieldmap node's on-screen
// size (view_art_px * zoom, in design units -- see apply_field_zoom below)
// always comes out to exactly 640x360: the full design canvas.
//
// That's the piece field_zoom alone could never do: setScale only rescales
// content whose visible extent (how much of the map the camera frames) was
// fixed regardless of zoom. At zoom < 2.0 the rescaled content no longer
// covers the 640x360 canvas -> black border on the uncovered edges. At
// zoom > 2.0 it overflows the canvas and gets cropped. Neither is what
// "adjust zoom, keep the screen filled" means.
//
// The three sites (disassembly-verified roles -- the two setScrollLimit
// sites were previously mislabeled here as view "densities"; they are not):
//
//   0x56d874  FieldMap::init -- the one TRUE view-size density:
//                 viewH_art (field+0x34c) = visibleH_design * (C / 320)
//             ui_scale_fix pins visibleH to 360; we want
//             viewH_art = 360/zoom, so C = 320/zoom. (+0x34c is consumed by
//             the per-frame Scroll() Y conversion "scrollY = Ycam + viewH -
//             planeH", which therefore becomes zoom-aware automatically.)
//
//   0x570cfc  FieldMap::setScrollLimit -- design->art unit conversion for
//             the X camera-limit overhang term:
//                 s0_art = (44.0 - originX) * C / 480
//             Stock C = 256 encodes the stock 1.875 design-per-art scale
//             (256/480 = 1/1.875). Under zoom the conversion is 1/zoom, so
//             C = 480/zoom. (The 44.0 literal itself -- a separate, stale
//             stock margin constant -- is NOT touched by this patch set.
//             It affects scroll clamping near map edges only, not general
//             mid-map centering -- see the (disabled) apply_field_camera_-
//             centering's header comment below for why that's a separate,
//             still-open problem, and where it actually needs fixing.)
//
//   0x570d0c  FieldMap::setScrollLimit -- scale factor for the Y camera-
//             limit top-overhang term K:
//                 K_art = (visibleH - 320) * C / 320 = 40 * C / 320 = C/8
//             K = viewH - 192: how far the DISPLAYED window's extent
//             exceeds the game-logic camera's own 192-art window, in the
//             scroll convention "scrollY = Ycam + viewH - planeH". Stock
//             identity: C = 192 -> K = 24 = 216 - 192, i.e. stock already
//             displays 24 art rows more than the engine window and K is
//             precisely that excess. Generalized: C = 8*(viewH - 192).
//             (The previous formula here, 4320/zoom - 2304, was derived
//             for the never-shipped symmetric-node design and reduces to
//             stock at NO configuration that ever ran -- it has been wrong
//             at every zoom since it was written, one more contributor to
//             edge-clamp weirdness at non-default zooms.)
//
// THE VERTICAL CENTERING MODEL (this is also where the character's screen
// height is fixed -- the bug where the character sat lower the more you
// zoomed in, unreachable below-center at 2.0 and feet-off-screen at 3.0):
//
//   FieldMap::Scroll converts the engine camera to the plane position every
//   frame as tY = camTopArt + viewH - planeH (disassembly- and log-
//   verified). With node_y = 0 and a screen that shows 360/zoom art rows,
//   that formula pins the VISIBLE window to
//         [camTop + viewH - 360/zoom,  camTop + viewH]
//   i.e. the visible BOTTOM sits at camTop + viewH always: viewH is not
//   just "how much is drawn", it POSITIONS the window. The recycler fills
//   rows [camTop, camTop + viewH] (proven by the round-1 node_y failure
//   geometry: node up at 1.75 -> bar at BOTTOM, node down at 2.0 -> bar at
//   TOP -- exactly the rows outside that range).
//
//   Stock (pixel-perfect off) displays viewH = 216 at y-scale 1.66667:
//   visible = [camTop, camTop+216], and ResetScrollAddress's 14-chip
//   constant puts the player's tile row at camTop + 112, so the character
//   renders 112 * 1.66667 = 186.67 design px from the screen top (~52%,
//   visually centered). THAT is the registration every zoom must
//   reproduce. (Multiple earlier sessions -- including the reverted
//   round-7 engine patch -- measured "stock" against the engine's internal
//   192-art window instead (112/192 = 58.3%). The engine window is not
//   what stock DISPLAYS; the display window is 216. This one wrong
//   reference is why every previous derivation landed low.)
//
//   The old formula here, viewH = 360/zoom, pins the visible TOP to camTop
//   instead -- so the character lands at 112*zoom design px: 210 at 1.875,
//   224 at 2.0, 336 at 3.0 (feet below screen). The exact reported
//   symptoms, linear in zoom.
//
//   Fix -- one computed number, no engine or node changes:
//         viewH = max(112 + 173.333/zoom,  360/zoom),  capped at 220
//     - zoom >= 1.6667 takes the first branch: the character's tile row
//       renders at exactly 186.67 design px AT EVERY ZOOM (identical to
//       stock pp-off), and field_zoom now extends the view above and below
//       that fixed point. Visible top = camTop + (112 - 186.67/zoom) stays
//       at/below camTop (drawn), and the visible bottom sits exactly at
//       the drawn bottom -- the same boundary the old formula already
//       shipped with no seam.
//     - zoom < 1.6667 takes the fill branch (= old behavior: top at
//       camTop, character slightly HIGH at 112*zoom design). The hard 220
//       cap is the 432x224 RenderTexture ceiling (five hardcoded create
//       sites in makeField): below zoom ~1.64 the screen simply needs more
//       art rows than the RT can hold and a top bar is unavoidable --
//       fixing zoom-out for real means resizing those five RT sites, a
//       separate project. This also explains the zoom 1.0 "map anchored to
//       the bottom, black above" look: visible bottom stays pinned to the
//       drawn bottom while the RT can't fill upward.
//
//   Identity: at zoom = 1.66667 BOTH vertical sites encode exactly the
//   stock 192.0f words -- bit-identical stock display geometry. (1.875 is
//   the X-scale identity; 1.66667 is the Y-scale identity. They were never
//   the same number -- that's the stock 9:8 art-pixel ratio again -- and
//   treating 1.875 as the Y invariant was the second load-bearing mistake
//   in the earlier derivations.)
//
// Reuses movz_topf (defined above, for apply_ui_scale_fix) directly --
// same single-instruction "movz w_,#topbits(float),lsl#16" encoding these
// sites already used for their original fixed values, so no new instruction
// slots are needed and no adjacent code is disturbed. Computed values are
// rounded to the nearest top-16 representable float (7 mantissa bits) before
// encoding: worst case ~0.5 art-px in viewH (~1 design px of character
// registration at 2x) -- and exact for the stock-identity 192.0 words.
// ---------------------------------------------------------------------------
static void apply_field_view_zoom(so_module *mod, float zoom) {
  if (zoom < 0.05f) zoom = 0.05f;  // guard: div-by-zero / degenerate view

  // Vertical: one source of truth. 173.333 = 360 - 186.67 = the art rows
  // that must sit BELOW the character's tile row, scaled back to art units.
  float viewH = 112.0f + 173.33333f / zoom;      // character-pinned branch
  float fill  = 360.0f / zoom;                   // screen-fill branch
  if (fill > viewH) viewH = fill;                // zoom < 1.6667
  if (viewH > 220.0f) viewH = 220.0f;            // 432x224 RT hard ceiling

  float density_v = viewH * (320.0f / 360.0f);   // 0x56d874: site stores C
                                                 //   where viewH = 360*C/320
  float kscale_v  = 8.0f * (viewH - 192.0f);     // 0x570d0c: C = 8*K
  float conv_h    = 480.0f / zoom;               // 0x570cfc: design->art, X

  // Round to nearest top-16 float (the movz slot keeps 7 mantissa bits).
  #define RN16(f) ({ union { float _f; uint32_t _u; } _x; _x._f = (f); \
                     _x._u = (_x._u + 0x8000u) & 0xFFFF0000u; _x._f; })
  density_v = RN16(density_v);
  kscale_v  = RN16(kscale_v);
  conv_h    = RN16(conv_h);
  #undef RN16

  const PatchEntry e[3] = {
    { NULL, 0, 0x56d874, movz_topf(8, 192.0f), movz_topf(8, density_v),
      "init: view-height density 192/320 -> (viewH*8/9)/320 [centering]" },
    { NULL, 0, 0x570d0c, movz_topf(9, 192.0f), movz_topf(9, kscale_v),
      "setScrollLimit: Y-limit overhang scale 192 -> 8*(viewH-192)" },
    { NULL, 0, 0x570cfc, movz_topf(8, 256.0f), movz_topf(8, conv_h),
      "setScrollLimit: X-limit design->art conversion 256 -> 480/zoom" },
  };
  apply_patches(mod, e, 3);
}

// Patch FieldMap::makeField's "fieldmap" node Node::setScale(zoom, zoom)
// call. Reuses the original 4 instruction slots (0x5761fc-0x576210) as:
//   movz w9, #lo16(zoom)
//   movk w9, #hi16(zoom), lsl #16
//   fmov s0, w9      (X)
//   fmov s1, w9      (Y -- same register, so always == X)
static void apply_field_zoom(so_module *mod, float zoom) {
  union { float f; uint32_t u; } x; x.f = zoom;
  uint16_t lo16 = (uint16_t)(x.u & 0xFFFF);
  uint16_t hi16 = (uint16_t)(x.u >> 16);

  const PatchEntry e[4] = {
    { NULL, 0, 0x5761fc, 0x528aaaa9, movz_w(9, lo16),
      "makeField: fieldmap node setScale: movz w9,#lo16(zoom)" },
    { NULL, 0, 0x576200, 0x1e2fd000, movk_w_hi(9, hi16),
      "makeField: fieldmap node setScale: movk w9,#hi16(zoom),lsl#16" },
    { NULL, 0, 0x576208, 0x72a7faa9, fmov_s_from_w(0, 9),
      "makeField: fieldmap node setScale X: fmov s0,w9" },
    { NULL, 0, 0x576210, 0x1e270121, fmov_s_from_w(1, 9),
      "makeField: fieldmap node setScale Y: fmov s1,w9" },
  };
  apply_patches(mod, e, 4);
}

// ---------------------------------------------------------------------------
// apply_field_scroll_anchor -- NOT CURRENTLY CALLED. See the long comment at
// its call site (in apply_game_patches, search "double-compensate") for why:
// this and apply_field_node_anchor fix the same pivot point two different,
// mutually-exclusive ways, and only one may be active at a time.
//
// fixes FieldMap::setScroll's map-entry camera
// snap so the player lands centered on screen at any zoom, not just stock
// 1.875. Supersedes the earlier apply_field_camera_centering attempt below
// (kept as history, not code -- see its post-mortem after this function).
//
// setScroll(x, y) is the one-time call that runs on room entry / warp. Its
// stock body (FieldMap::setScroll, 0x576cd8, disassembly-verified against
// this exact binary) computes:
//     tX = 128.0 - x
//     tY = (y + 96.0) - planeH        (planeH = this+0x334, render-plane
//                                       height in art-px, read as int and
//                                       scvtf'd to float)
// and writes the pair into THREE places each (+0x350/+0x358/+0x360 for tX,
// +0x354/+0x35c/+0x364 for tY -- a degenerate min/cur/max triplet, all equal
// at map-entry; setScrollLimit widens min/max later for rooms bigger than
// one screen). 128.0 and 96.0 are fixed HALF-VIEWPORT constants in art-px:
// half of the original pre-zoom 256x192 art window (256/2=128, 192/2=96).
// They are correct ONLY at the stock 1.875 zoom the layout was authored
// for (240/1.875=128, 180/1.875=96 -- 240x180 being half of the 640x360
// design canvas) and never scale with config.field_zoom, which is exactly
// why the character lands off-center by a constant, zoom-proportional
// amount at any other zoom (empirically +16,+12 px at zoom=2, -16,-12 px
// at zoom=1.75 -- both confirmed against real debug logs).
//
// Fix: replace the two half-viewport constants with 240.0/zoom and
// 180.0/zoom, generalizing the same relationship that produces the stock
// 128/96 values at zoom=1.875. This is dimensionally the same fix already
// applied (independently) to the view-size and camera-limit sites in
// apply_field_view_zoom above -- setScroll was simply the one site that
// investigation hadn't reached yet.
//
// How it's applied: the ORIGINAL 14 instructions at 0x576d28-0x576d5c (56
// bytes -- everything from the first constant load through the last of the
// six store instructions) are replaced in place with 14 new instructions
// that compute the same tX/tY pair from zoom-scaled constants instead of
// fixed ones, using the exact same input registers (s8=y-arg, s9=x-arg,
// x19=this) and output locations (+0x350.. via x9=this+0x350) as stock.
// No new instruction slots, no branch, no cave -- every slot's *old* word
// was read directly from this build's libchrono.so and is verified before
// writing, so a future binary that doesn't match this exact layout will
// skip the patch with a logged mismatch instead of corrupting anything.
//
// Reordering (why 14 slots in, 14 out, nothing left over):
//   old 0x576d28 mov w8,#bits(96.0)        -> new movz w8,#bits(180/zoom)
//   old 0x576d2c ldr s1,[x19,#0x334]       -> unchanged (loads planeH int)
//   old 0x576d30 movi v0.2s,#0x43,lsl#24   -> new fmov s2,w8   (moved from old d34)
//   old 0x576d34 fmov s2,w8                -> new movz w8,#bits(240/zoom)
//   old 0x576d38 scvtf s1,s1               -> unchanged
//   old 0x576d3c fadd s2,s8,s2             -> unchanged
//   old 0x576d40 fsub s0,s0,s9             -> new fmov s0,w8  (new)
//   old 0x576d44 fsub s1,s2,s1             -> new fsub s0,s0,s9   (moved from old d40)
//   old 0x576d48 str s0,[x19,#0x350]       -> new fsub s1,s2,s1   (moved from old d44)
//   old 0x576d4c str s0,[x19,#0x358]       -> new add x9,x19,#0x350   (new)
//   old 0x576d50 str s0,[x19,#0x360]       -> new stp s0,s1,[x9]       (new)
//   old 0x576d54 str s1,[x19,#0x354]       -> new stp s0,s1,[x9,#8]    (new)
//   old 0x576d58 str s1,[x19,#0x35c]       -> new stp s0,s1,[x9,#0x10] (new)
//   old 0x576d5c str s1,[x19,#0x364]       -> new nop                  (6th store was redundant)
// Every "new" line was hand-assembled with Keystone and round-tripped
// through Capstone to confirm it decodes back to exactly the intended
// mnemonic/operands before being hardcoded here.
//
// What this does NOT touch, and why: FieldMap::Scroll (0x576ec8, the real
// ~10KB per-frame camera-follow function) was fully disassembled and
// searched for any embedded copy of 128.0/96.0/240.0-family constants in
// its general (non-cutscene) path -- none exist. The only 240.0/360.0 pair
// found in Scroll() lives behind a `scroll-mode == 3` branch (a scripted
// pan, operating in fixed DESIGN-space units that are correct at any zoom
// by construction) and is unrelated to normal player-follow. That's
// consistent with Scroll() re-deriving its per-frame target purely from
// the persistent +0x350.. fields this patch writes (and from the already
// zoom-aware view-size fields apply_field_view_zoom feeds elsewhere) rather
// than hardcoding its own copy of the anchor -- which would mean this one
// fix is sufficient for continuous walking too, not just the room-entry
// snap. That inference is NOT yet confirmed on real hardware: it's a
// reasonable reading of the disassembly, not a substitute for new
// debug_zoom_*.log captures with this patch active. Do that before
// declaring the centering bug closed. FieldMap::Scroll itself is
// deliberately left byte-for-byte untouched here regardless -- every prior
// attempt to patch it directly has shipped a new, different visible bug
// (black borders, shifted/cut-off content, a crash), documented in the
// apply_field_camera_centering post-mortem below.
// ---------------------------------------------------------------------------
__attribute__((unused))
static void apply_field_scroll_anchor(so_module *mod, float zoom) {
  if (zoom < 0.05f) zoom = 0.05f;  // guard: div-by-zero / degenerate anchor

  const uint32_t wY = movz_topf(8, 180.0f / zoom);
  const uint32_t wX = movz_topf(8, 240.0f / zoom);

  const PatchEntry e[14] = {
    { NULL, 0, 0x576d28, 0x52a85808, wY,
      "setScroll: Y-half-viewport const: movz w8,#96.0 -> #(180/zoom)" },
    { NULL, 0, 0x576d2c, 0xbd433661, 0xbd433661,
      "setScroll: ldr s1,[x19,#0x334] (planeH) -- unchanged" },
    { NULL, 0, 0x576d30, 0x0f026460, fmov_s_from_w(2, 8),
      "setScroll: X-half-viewport const load moved here: fmov s2,w8 (Y)" },
    { NULL, 0, 0x576d34, 0x1e270102, wX,
      "setScroll: X-half-viewport const: movz w8,#128.0(vec) -> #(240/zoom)" },
    { NULL, 0, 0x576d38, 0x5e21d821, 0x5e21d821,
      "setScroll: scvtf s1,s1 (planeH int->float) -- unchanged" },
    { NULL, 0, 0x576d3c, 0x1e222902, 0x1e222902,
      "setScroll: fadd s2,s8,s2 (y_arg + Y-half-viewport) -- unchanged" },
    { NULL, 0, 0x576d40, 0x1e293800, fmov_s_from_w(0, 8),
      "setScroll: fmov s0,w8 (X-half-viewport, new slot)" },
    { NULL, 0, 0x576d44, 0x1e213841, 0x1e293800,
      "setScroll: fsub s0,s0,s9 (X-half-viewport - x_arg = tX), moved" },
    { NULL, 0, 0x576d48, 0xbd035260, 0x1e213841,
      "setScroll: fsub s1,s2,s1 (Y-sum - planeH = tY), moved" },
    { NULL, 0, 0x576d4c, 0xbd035a60, add_x_imm(9, 19, 0x350),
      "setScroll: add x9,x19,#0x350 (base of tX/tY min/cur/max triplet)" },
    { NULL, 0, 0x576d50, 0xbd036260, stp_s_imm(0, 1, 9, 0x00 / 4),
      "setScroll: stp s0,s1,[x9]       (Xmin,Ymin)" },
    { NULL, 0, 0x576d54, 0xbd035661, stp_s_imm(0, 1, 9, 0x08 / 4),
      "setScroll: stp s0,s1,[x9,#8]    (Xcur,Ycur)" },
    { NULL, 0, 0x576d58, 0xbd035e61, stp_s_imm(0, 1, 9, 0x10 / 4),
      "setScroll: stp s0,s1,[x9,#0x10] (Xmax,Ymax)" },
    { NULL, 0, 0x576d5c, 0xbd036661, 0xd503201f,
      "setScroll: nop (6th store was redundant -- Ymax now set by the stp above)" },
  };
  apply_patches(mod, e, 14);
}

static uint32_t encode_b(uintptr_t from_addr, uintptr_t to_addr); // forward declare
// ---------------------------------------------------------------------------
// apply_field_node_anchor -- fixes FieldMap::makeField's setPosition call so
// the fieldmap NODE's own screen position pivots around design-canvas center
// (320,180) at any zoom, instead of the stale flat (ctr::x_offset, 0.0) pair
// (a leftover multi-device-width formula that only ever evaluates to a fixed
// +80/+0 offset once ui_scale_fix pins the canvas to 640x360 --
// blind to zoom entirely).
//
// This uses the exact same local reference point, (128,96), that
// apply_field_scroll_anchor above already made setScroll's camera-follow AIM
// point pivot around (via its 240/zoom, 180/zoom formula, which is just
// 2x128/2x96 in the same units). That match is not a coincidence: 128,96 is
// this engine's original half-viewport constant (half of the stock 256x192
// view) -- both setScroll (where the camera AIMS, in world space) and the
// node's own position (where that aimed content actually LANDS on screen)
// were always meant to agree on this same anchor. Before this fix, only one
// of the two had been corrected for zoom, so they disagreed at every zoom
// except the one value (1.875) where the old code's numbers happened to
// coincide -- which is exactly why "even 2.0 wasn't centered" despite
// setScroll already being fixed.
//
//   node_x = 320.0 - 128.0*zoom
//   node_y = 180.0 - 96.0*zoom
//
// At zoom=1.875 this reduces to exactly (80.0, 0.0) -- bit-identical to the
// stock runtime values (ctr::x_offset evaluates to 80.0 under our fixed
// 640x360 canvas; the old Y arg was already a hardcoded 0.0) -- so there is
// no behavior change at the reference zoom, same as every other zoom patch
// in this file.
//
// Why this attempt is different from the disabled apply_field_camera_centering
// below (which caused visible corruption -- stretched/stale plane content):
// that attempt moved the node BEFORE setScroll's own camera-follow constants
// had been corrected, so the node's new position and the camera's actual aim
// point disagreed with each other in a new way, on top of the plane/RT
// render-window mismatch. Now that apply_field_scroll_anchor has already
// made setScroll pivot around this same (128,96) point, moving the node to
// pivot around the SAME point keeps both systems in agreement -- the amount
// the node needs to shift to recenter is small (single-digit to low-double-
// digit art-px at the zoom range 1.25-3.0) and well within the fixed 432x224
// RT canvas's redraw margin (independently re-verified against this exact
// binary: the five RenderTexture::create call sites in makeField are still
// hardcoded 432x224 -- confirmed fresh, not assumed from an old session).
// Zoom below ~1.65 already has a separate, known, pre-existing limitation
// (the view itself exceeds what a 432x224 canvas can hold, independent of
// this fix) -- not something this patch changes either way.
//
// UPDATE (this session, round N): node_y is tunable now, not a fixed value.
// History of what didn't work, briefly: (1) node_y = 180-96*zoom -- exposed
// unrendered plane area (asymmetric margin: ~9-22 art-px vertically vs
// ~33-56 horizontally at the zoom range that matters, so the same-size shift
// that's safe for X isn't for Y -- black bar at BOTTOM at 1.75, TOP at 2.0).
// (2) patching FieldMap::Scroll's per-frame tY sum directly -- desynced the
// background plane from the sprite layer, since tY is a derived intermediate
// consumed at different points by different layers (confirmed from your
// screenshots: map and character stopped moving together while walking).
// (3) patching FieldImpl::ResetScrollAddress's chip-window constants (the
// upstream game-logic camera seed) -- mechanically the RIGHT layer to patch
// (avoids the desync from (2)), but re-deriving its exact effect on final
// on-screen position requires correctly relating THREE independent unit
// systems (engine "chip" window ratios, the render-side art-pixel view
// height, and the outer zoom transform) that did not converge to one
// trustworthy formula through disassembly alone -- worth noting, one clue
// found along the way: the engine's window places the character at ~58% down
// the screen, not 50%, meaning the STOCK/native game was likely never at
// exact geometric center to begin with, which is plausibly part of why a
// "make it hit exactly 180" derivation kept landing short.
//
// (4) exposing the correction as two tunable config values
// (field_zoom_y_center, field_zoom_y_coeff) with an internal margin clamp,
// on the theory that a badly-tuned coefficient could only under-correct and
// never reproduce the black-bar bug from (1). Measured directly against
// real on-device screenshots at zoom 1.0/2.0/3.0, this was WRONG: at
// zoom=3.0 the clamp let through a shift roughly 3x larger than the actual
// available margin (144 design-units computed vs. a real ~30-unit black bar
// appearing on screen), and at zoom=2.0 the "safely clamped" 36-unit shift
// produced a 36-unit black bar directly. The clamp's margin model did not
// match the real, in-practice redraw margin -- so this is not a case of
// picking better constants; the mechanism itself (shifting node_y, which
// also shifts the background since it's the same parent node) reintroduces
// the exact class of bug it was meant to safely avoid.
//
// (5) a raw, unclamped, zoom-independent design-unit value
// (field_zoom_y_test_shift) added directly to node_y, on the theory that at
// least a SMALL value would be safe even if the exact safe limit couldn't
// be calculated. Also wrong on real hardware -- because the mechanism (not
// the amount) is the problem: even a small shift of THIS node moves the
// same parent the background renders from, and the background's own
// redraw system doesn't know the shift happened.
//
// CONCLUSION after 5 rounds: node_y is retired as a lever for this axis
// entirely, permanently fixed at 0.0 (see below) -- not because a small
// enough number couldn't be found, but because this specific node is the
// wrong place to make ANY vertical correction (shifting it either does
// nothing or reintroduces black bars, confirmed on real hardware -- there
// is no safe middle value).
//
// Round 6 tried moving the correction upstream instead, to
// apply_field_engine_center_y (FieldImpl::ResetScrollAddress's camera-
// placement ratio) -- also disabled after real-hardware testing made things
// WORSE, not better (see that function's own header for what was wrong
// with the reasoning). As of this session, Y has NO working fix. X is
// untouched throughout -- proven correct already, ample margin at every
// zoom, never had this problem.
//
// Implementation note: of the 6 instruction slots between the setScale call
// and setPosition's blr, only 2 are actually free to repurpose. The other 4
// -- including BOTH halves of resolving x21 = &ctr::x_offset -- must stay
// completely untouched, because x21 is read a SECOND time, ~700 bytes later
// at 0x57652c, by a different (overlay/border) node's setPosition call that
// this patch is not touching. This is exactly the register-liveness mistake
// that caused a save-load crash in a much earlier session; it's called out
// explicitly here because it was re-verified against this exact binary
// before writing a single byte, not discovered by testing. The two free
// slots (0x576220, was `movi d1,#0`; 0x576230, was `ldr s0,[x21]`) become a
// branch-out to a small cave (compute node_x with full movz+fmov precision,
// set s1 = 0.0 via a single `fmov s1,wzr`) and a neutralizing nop,
// respectively -- x21's own resolution at 0x57621c/0x576228 is left 100%
// byte-identical to stock.
// ---------------------------------------------------------------------------
static void apply_field_node_anchor(so_module *mod, float zoom) {
  if (zoom < 0.05f) zoom = 0.05f;  // guard: div-by-zero / degenerate anchor

  const uint32_t CAVE_CODE  = 0x376394;  // verified all-zero, 32B, R+X segment,
                                          // unused by any other patch in this file
  const uint32_t BRANCH_OUT = 0x576220;  // was: movi d1,#0 (dead: old Y-arg=0.0)
  const uint32_t RESUME     = 0x576224;  // mov x0,x26 -- UNCHANGED, resumes here
  const uint32_t NOP_SITE   = 0x576230;  // was: ldr s0,[x21] (dead: old X-arg)

  uintptr_t base        = (uintptr_t)mod->load_base;
  uintptr_t code_addr    = base + CAVE_CODE;
  uintptr_t branch_addr  = base + BRANCH_OUT;
  uintptr_t resume_addr  = base + RESUME;

  // --- X: unchanged from before -- proven correct, ample margin ---
  const float node_x = 320.0f - 128.0f * zoom;

  // --- Y: back to a FIXED 0.0. Retired for good this time. ---
  //
  // Every attempt at correcting Y by shifting THIS node (fixed derivation,
  // engine-constant-based derivation, per-frame Scroll() patch, tunable
  // formula with a margin clamp, then a raw unclamped test value) hit the
  // same wall, confirmed directly on real hardware: this node is the SAME
  // parent the background renders from, so any shift big enough to matter
  // either does nothing (the background's own redraw system just re-centers
  // under it) or reintroduces a black bar (exposing area the background
  // never redrew for the new position) -- there is no safe middle value to
  // dial in, because the mechanism itself is wrong, not the constant.
  //
  // Round-6 (apply_field_engine_center_y) and round-7
  // (apply_field_engine_registration_y) both moved the correction upstream
  // to FieldImpl::ResetScrollAddress instead -- both disabled after
  // real-hardware testing (round 7 additionally broke the battle camera;
  // see its post-mortem). Y's actual fix lives in apply_field_view_zoom's
  // viewH formula now -- the render-side window position, not this node
  // and not the engine. X stays exactly as-is here -- it never had this
  // problem (ample, confirmed-real margin at every zoom).
  const float node_y = 0.0f;

  uint32_t off = 0;
  #define EMIT(w) do { uint32_t _w = (w); __builtin_memcpy((void*)(code_addr+off), &_w, 4); off += 4; } while (0)
  EMIT(movz_topf(9, node_x));     // w9 = node_x bit pattern
  EMIT(fmov_s_from_w(0, 9));      // s0 = node_x
  EMIT(movz_topf(10, node_y));    // w10 = node_y bit pattern
  EMIT(fmov_s_from_w(1, 10));     // s1 = node_y
  EMIT(encode_b(code_addr + off, resume_addr));  // back into makeField
  #undef EMIT

  const PatchEntry e[2] = {
    { NULL, 0, BRANCH_OUT, 0x2f00e401, encode_b(branch_addr, code_addr),
      "makeField: setPosition X/Y args -- branch to node-anchor cave (was: movi d1,#0)" },
    { NULL, 0, NOP_SITE, 0xbd4002a0, 0xd503201f,
      "makeField: old ctr::x_offset X-arg read -- neutralized to nop (x21 itself "
      "is left fully intact -- its OTHER, later use at 0x57652c for an unrelated "
      "overlay node still needs it, and is untouched by this patch)" },
  };
  apply_patches(mod, e, 2);
}

// ---------------------------------------------------------------------------
// apply_field_scroll_pivot_y -- DISABLED, post-mortem only (not called
// anywhere -- kept for history, same as apply_field_camera_centering and
// apply_field_scroll_anchor above).
//
// What it did: corrected FieldMap::setScroll's Y-half-viewport constant
// (96.0, half of the stock 192-art-px view height) to 180.0/zoom, on the
// theory that this was the mechanism controlling the character's vertical
// screen position. It was written when apply_field_node_anchor still left
// node_y permanently at 0.0, under the (at-the-time reasonable) assumption
// that Y needed a DIFFERENT mechanism than X's node-shift, since node-
// shifting Y turned out to have very little spare render margin.
//
// Why it's superseded, not just redundant: setScroll's Y-half-viewport
// constant only affects the ONE-TIME camera snap on room entry --
// FieldMap::Scroll() recomputes tY from its own separate internal constant
// every single frame afterward, completely independent of this one. So this
// patch's effect was real for exactly one frame per room entry and then
// silently overwritten -- invisible during ordinary walking, which is
// consistent with testing that showed no measurable difference with it on
// vs. off. The actual, persistent Y correction now lives in
// apply_field_node_anchor's tunable node_y trim (config.field_zoom_y_center
// / field_zoom_y_coeff), applied in the OUTER/post-scale frame the same way
// X's correction always has been -- one mechanism per axis, both proven
// against the actual redraw margin rather than assumed.
// ---------------------------------------------------------------------------
static void apply_field_scroll_pivot_y(so_module *mod, float zoom) __attribute__((unused));
static void apply_field_scroll_pivot_y(so_module *mod, float zoom) {
  if (zoom < 0.05f) zoom = 0.05f;
  const uint32_t SITE = 0x576d28;  // was: mov w8,#0x42c00000 (96.0)
  const PatchEntry e[1] = {
    { NULL, 0, SITE, 0x52a85808, movz_topf(8, 180.0f / zoom),
      "setScroll: Y-half-viewport pivot 96.0 -> 180.0/zoom (X pivot at "
      "0x576d30, still stock 128.0, deliberately untouched)" },
  };
  apply_patches(mod, e, 1);
}

// ---------------------------------------------------------------------------
// apply_field_engine_center_y -- DISABLED. Patches FieldImpl::ResetScrollAddress,
// the game engine's camera-placement code that runs once every time you
// enter a room, on the theory that its 14/24 chip window split directly sets
// the character's on-screen vertical registration.
//
// Disassembly of the general/mid-map case (0x576e98-0x576ea8), confirmed
// fresh against this exact binary and still accurate:
//   w9 = 2*player_tile_y
//   window_top    = w9 - 14   (stored to state+0x18)
//   window_bottom = w9 + 10   (stored to state+0x1c)
// 1 chip = 8 native-resolution art-pixels; 24 chips = 192, the original
// pre-zoom field view height. 14/24 = 58.3% down the window.
//
// WHY THIS IS DISABLED: the encoding analysis above was real and re-verified,
// but it only established what WRITES state+0x18/+0x1c (confirmed: nothing
// else does, except a screen-shake subsystem gated off during normal
// walking). It never established what READS those two fields or how they
// actually turn into a screen position -- that gap was the mistake. Tested
// on real hardware at chips=12 (the exact-50/50 value), zoom=3.0: the
// character got WORSE, not better -- lower on screen, with the bottom of
// the screen clipping/going missing. That result only makes sense if this
// window ISN'T a direct registration point for the character sprite.
//
// Re-reading the FULL function (not just the mid-case branch) afterward:
// every case, on both axes, produces a [min,max] pair with the exact same
// shape a scroll-limit / edge-clamp / dead-zone range would have -- boundary
// checks against state+0x38/0x3c/0x40/0x44 first, then a window is written
// covering wherever the player currently is. That's consistent with "this
// is the camera's allowed scroll range," not "this is where the character
// sits on screen." The actual steady-state screen position most likely
// still comes from FieldMap::Scroll()'s per-frame tY computation (the one
// with the baked-in half-view constant found earlier in this project) --
// which was tried and reverted for a DIFFERENT reason (it desynced the
// background from the character, since it's a downstream copy consumed at
// a different point than the background redraw). Whether some third,
// still-unfound piece ties these two functions together is unknown.
//
// Left in place (not deleted) as documented, working history of what does
// NOT work and why, consistent with every other retired attempt in this
// file. Not called anywhere; kept `static` + `__attribute__((unused))` so
// it doesn't need to be deleted to keep the build clean.
// ---------------------------------------------------------------------------
static void apply_field_engine_center_y(so_module *mod, int chips) __attribute__((unused));
static void apply_field_engine_center_y(so_module *mod, int chips) {
  if (chips < 0) chips = 0;
  if (chips > 24) chips = 24;
  const int below = 24 - chips;

  const uint32_t SITE_TOP = 0x576e9c;  // was: sub w10,w9,#0xe  (14)
  const uint32_t SITE_BOT = 0x576ea4;  // was: add w10,w9,#0xa  (10)

  const PatchEntry e[2] = {
    { NULL, 0, SITE_TOP, 0x5100392a, 0x51000000u | ((uint32_t)(chips & 0xFFF) << 10) | (9u << 5) | 10u,
      "ResetScrollAddress: window-top chip offset 14 -> field_zoom_y_anchor_chips" },
    { NULL, 0, SITE_BOT, 0x1100292a, 0x11000000u | ((uint32_t)(below & 0xFFF) << 10) | (9u << 5) | 10u,
      "ResetScrollAddress: window-bottom chip offset 10 -> 24-anchor_chips" },
  };
  apply_patches(mod, e, 2);
}

// ---------------------------------------------------------------------------
// apply_field_camera_centering -- DISABLED, post-mortem only (superseded by
// apply_field_node_anchor above -- kept for history, not called anywhere).
//
// What it tried to do: reposition the fieldmap node itself (node_x =
// 320-128*zoom, node_y = 180-96*zoom) so the node's own local origin landed
// on the design-canvas center at any zoom, replacing the stale flat
// (ctr::x_offset, 0.0) pair makeField normally passes to setPosition.
//
// NOTE: apply_field_node_anchor above ends up using this exact same formula
// -- the formula was always right. What was missing back when this was
// first tried is what's described below: setScroll's own camera-follow
// constants (fixed later by apply_field_scroll_anchor) hadn't been corrected
// yet, so moving the node and the camera's actual aim point disagreed with
// each other on top of the render-window issue. See apply_field_node_anchor's
// header comment above for the full current reasoning and the register-
// liveness bug (the x21 double-use at 0x57652c) that a naive "just redo the
// same patch" attempt would have silently reintroduced.
//
// Why it's wrong (as tried THEN, before apply_field_scroll_anchor existed):
// the background art for a map is NOT drawn live into the node's full local
// space -- it's pre-rendered into a fixed-size RenderTexture ("plane",
// historically measured at 432x224 art-px for standard maps) that is itself
// a CHILD of the fieldmap node, repositioned every frame by FieldMap::Scroll
// independently, using its own internal scrollX/scrollY camera-follow state
// -- with no knowledge of any outer offset applied to the parent node.
// Moving the parent node doesn't "recenter the camera" at all by itself: it
// just physically slides the already-rendered, fixed-size picture around
// under a canvas that's still exactly 640x360. Slide it far enough (as it
// was, back when setScroll's own aim point was still wrong) and you uncover
// plane area the ring-buffer tile-recycler never redrew for the current
// scroll position -- which is exactly "sprites at the top look stretched
// into vertical lines, like peeking past what we should see": that's stale/
// edge-clamped texture content becoming visible, not a stretching of the
// sprites themselves.
//
// 0x576230 (`ldr s0,[x21]`) and everything else in FieldMap::makeField's
// setPosition call is left 100% untouched, restoring the exact stock
// (ctr::x_offset, 0.0) behavior.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Scroll-state diagnostic snapshot -- v2.
//
// v1 hooked FieldMap::Scroll()'s entry and called debugPrintf() (mutexLock
// -> vfprintf -> fflush) directly from inside the hook. That crashed on
// save-resume, twice, in different specific ways, even after each version
// passed careful static verification (instruction encoding, branch targets,
// register liveness, cave-zero/collision checks -- all confirmed correct
// both times). A static bug that keeps getting fixed and the crash keeps
// recurring the same way points at something static analysis can't see.
// The most likely candidate: FieldMap::Scroll() (or whatever calls it) may
// run on an engine-spawned thread rather than the homebrew's own main
// thread. This project's pthread shim (tls_setup_guard() in so_util.c)
// gives such threads a synthetic TLS block that is NOT a valid libnx
// ThreadVars struct -- harmless for the engine's own code, which never
// touches libnx, but fatal the moment something calls a real libnx
// primitive from that thread. mutexLock() (used inside debugPrintf) is
// exactly such a primitive.
//
// v2 removes that risk category entirely rather than chasing it further:
// the hook body does ONLY raw memory reads (from the FieldMap 'this'
// pointer -- always valid, it's a normal C++ method call) and raw memory
// writes to a plain global struct in the HOMEBREW's own process memory.
// No calls, no branches into any library code, no mutex, no file I/O --
// literally just loads and stores, so it can't care what thread it runs
// on. Verified: the hook body touches only x9/x10/sp -- x0 (this), s0
// (the incoming float arg), and x30 (return address) are never written,
// so there's nothing to save/restore at all, which also removes the whole
// class of "did I preserve state across a call correctly" bug that hit
// v1 twice.
//
// The actual debugPrintf() call happens separately, once per real frame,
// from ct_nx's own main loop in main.c (see scroll_dbg_drain(), called
// right after e_nativeRender()) -- a context that has called debugPrintf
// safely for every "patches: ..." line in every boot log this whole
// session, so there's no new risk introduced there.
//
// Logs the same six scroll-state fields as before: the per-frame
// camera-follow target (+0x350/+0x354), the interpolated "current" value
// (+0x358/+0x35c), and the final derived pair (+0x368/+0x36c). Reading
// them at Scroll()'s entry means we see exactly what last frame finished
// with -- real end-to-end state, not a mid-computation snapshot.
//
// This is read-only with respect to game logic -- it changes nothing
// about how the camera actually behaves, only what gets written to
// debug.log. Remove the call to apply_field_scroll_debug_log() (and
// the drain call in main.c) once the drift is diagnosed.
// ---------------------------------------------------------------------------

// Encode: movz x<rd>, #imm16, lsl #(16*hw)   (64-bit form, hw = 0..3)
static uint32_t movz_x(int rd, uint16_t imm16, int hw) {
  return 0xD2800000u | ((uint32_t)(hw & 3) << 21) | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}
// Encode: movk x<rd>, #imm16, lsl #(16*hw)
static uint32_t movk_x(int rd, uint16_t imm16, int hw) {
  return 0xF2800000u | ((uint32_t)(hw & 3) << 21) | ((uint32_t)imm16 << 5) | (uint32_t)rd;
}
// Encode: b <target>  (unconditional, PC-relative, +-128MB range)
static uint32_t encode_b(uintptr_t from_addr, uintptr_t to_addr) {
  int64_t off = (int64_t)to_addr - (int64_t)from_addr;
  uint32_t imm26 = (uint32_t)((off / 4) & 0x3FFFFFF);
  return 0x14000000u | imm26;
}
// Write 4 instructions that load a 64-bit absolute address into x<rd>.
static void write_abs64(uintptr_t at, int rd, uint64_t val) {
  uint32_t words[4] = {
    movz_x(rd, (uint16_t)(val >>  0), 0),
    movk_x(rd, (uint16_t)(val >> 16), 1),
    movk_x(rd, (uint16_t)(val >> 32), 2),
    movk_x(rd, (uint16_t)(val >> 48), 3),
  };
  for (int i = 0; i < 4; i++)
    __builtin_memcpy((void *)(at + i * 4), &words[i], 4);
}
// Encode: ldr w<rt>, [x<rn>, #byte_off]  (byte_off must be a multiple of 4, 0..16380)
static uint32_t ldr_w_imm(int rt, int rn, uint32_t byte_off) {
  return 0xB9400000u | (((byte_off / 4) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
// Encode: str w<rt>, [x<rn>, #byte_off]
static uint32_t str_w_imm(int rt, int rn, uint32_t byte_off) {
  return 0xB9000000u | (((byte_off / 4) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
// Encode: add w<rd>, w<rn>, #imm12
static uint32_t add_w_imm(int rd, int rn, uint32_t imm12) {
  return 0x11000000u | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
// (ldr_x_imm / add_x_imm12_lsl12 / cbz_x encoders removed here -- they only
// existed to support the offX/offY and ChronoCanvas-accumulator reads that
// were cut from apply_field_scroll_debug_log after repeated crashes despite
// passing every static check available. If a future attempt needs a
// pointer-indirection read again, the encodings were: ldr x<rt>,[x<rn>,#off]
// = 0xF9400000 | (((off/8)&0xFFF)<<10) | (rn<<5) | rt; add x<rd>,x<rn>,
// #imm12,lsl#12 = 0x91400000 | ((imm12&0xFFF)<<10) | (rn<<5) | rd; cbz
// x<rt>,<byte_offset> = 0xB4000000 | (((byte_offset/4)&0x7FFFF)<<5) | rt.)

// Lives in the HOMEBREW's own process memory (ct_nx.elf), NOT inside
// libchrono.so -- so_finalize() locks the .so's executable segment
// Read+Execute-only early in boot and never makes it writable again; a
// mutable cell would have to live outside it regardless of what writes to
// it. seq is bumped every time the hook runs; main.c's drain function
// polls it to know when there's new data to print, and to avoid ever
// printing the same frame's data twice.
typedef struct {
  volatile uint32_t seq;
  float tX, tY;   // per-frame camera-follow target  (+0x350/+0x354)
  float cX, cY;   // interpolated "current" value    (+0x358/+0x35c)
  float fX, fY;   // final derived pair               (+0x368/+0x36c)
  // Added while chasing the "final pair looks like a near-constant offset"
  // finding below: the render-plane extent (historically measured
  // 432x224 art-px -- see apply_field_camera_centering's disabled-function
  // comment) and the screen-shake/offset pair that fX/fY are built from.
  // Disassembly of the real Scroll() in this build shows fX/fY are NOT a
  // simple function of tX/tY/cX/cY alone -- they also fold in planeW/planeH
  // (+0x330/+0x334) and this offset pair, plus three literal immediates
  // (-64.0, 216.0, 256.0) that apply_field_view_zoom's zoom-rescaling never
  // touched. Capturing these lets us tell, from real logs at different
  // zoom settings, whether those literals need to become zoom-dependent
  // too -- rather than guessing a fourth unverified change to this
  // register-heavy function after the last three each shipped a new,
  // different visible bug (see that comment for the history).
  int32_t planeW, planeH;   // +0x330/+0x334, render-plane extent (art px)
  // (Note: the fY = 360/zoom - 256 formula and fX's constant -64.0 were
  // already fully confirmed from the five existing logs without needing
  // offX/offY -- so nothing analytical is lost by cutting them.)
  // REMOVED (this pass): offX/offY (via this->+0x458->+0xf88/+0xf8c) and
  // the 8-field ChronoCanvas accumulator block (via mpInstance->+0x134f0
  // range). Both were re-verified as statically sound -- I traced the
  // REAL Scroll() in this exact binary and confirmed it dereferences
  // this->+0x458 completely unconditionally with no null-check as
  // literally its first action after the prologue (so that pointer is
  // always valid whenever Scroll() runs at all), and confirmed
  // ChronoCanvas is a real ~118KB allocation (found its actual `operator
  // new` call site: 0x1CFF8 bytes), so the accumulator reads are nowhere
  // near out-of-bounds either. Every static check available says both
  // additions should be safe -- and the build still crashed on save
  // resume regardless. Rather than add a fourth or fifth "verified but
  // still crashes" layer on top of a technique that has now failed
  // repeatedly despite passing every check I know how to run, both are
  // cut here. The remaining fields below are exactly the set that
  // already produced five clean, real, crash-free logs across zoom
  // 1.0-3.0 in an earlier session -- known-good, not re-guessed.
} ScrollDbgSnapshot;
static ScrollDbgSnapshot s_scroll_dbg_snap = {0};

static void apply_field_scroll_debug_log(so_module *mod) {
  // Same cave as before -- independently re-verified all-zero and inside
  // the R+X segment for this exact binary. This is back down to the
  // minimal field set (tX/tY/cX/cY/fX/fY/planeW/planeH, ~30 instructions
  // / ~120 bytes total) -- the offX/offY and ChronoCanvas-accumulator
  // additions have been removed after repeated crashes despite passing
  // every static check available (see ScrollDbgSnapshot's comment above).
  const uint32_t CAVE_CODE = 0xd0118;
  const uint32_t ENTRY      = 0x576ec8;   // FieldMap::Scroll first instr
  const uint32_t RESUME     = 0x576ecc;   // instr right after it

  uintptr_t base        = (uintptr_t)mod->load_base;
  uintptr_t code_addr    = base + CAVE_CODE;
  uintptr_t entry_addr   = base + ENTRY;
  uintptr_t resume_addr  = base + RESUME;

  // Struct field byte offsets -- computed by THIS compiler from the real
  // struct, not hand-derived, so they can't drift out of sync with
  // whatever padding/layout the compiler actually chose.
  const uint32_t OFF_SEQ    = (uint32_t)offsetof(ScrollDbgSnapshot, seq);
  const uint32_t OFF_TX     = (uint32_t)offsetof(ScrollDbgSnapshot, tX);
  const uint32_t OFF_TY     = (uint32_t)offsetof(ScrollDbgSnapshot, tY);
  const uint32_t OFF_CX     = (uint32_t)offsetof(ScrollDbgSnapshot, cX);
  const uint32_t OFF_CY     = (uint32_t)offsetof(ScrollDbgSnapshot, cY);
  const uint32_t OFF_FX     = (uint32_t)offsetof(ScrollDbgSnapshot, fX);
  const uint32_t OFF_FY     = (uint32_t)offsetof(ScrollDbgSnapshot, fY);
  const uint32_t OFF_PLANEW = (uint32_t)offsetof(ScrollDbgSnapshot, planeW);
  const uint32_t OFF_PLANEH = (uint32_t)offsetof(ScrollDbgSnapshot, planeH);

  // Register read/write-set check (re-run this pass) confirms x0, s0, and
  // x30 are never written anywhere in the body -- only x9, x10, sp are.
  // This is exactly the field set (tX/tY/cX/cY/fX/fY/planeW/planeH), all
  // direct offsets on 'this' with zero pointer-chasing, that already
  // produced five clean, crash-free logs in an earlier session.
  uint32_t off = 0;
  #define EMIT(w) do { uint32_t _w = (w); __builtin_memcpy((void*)(code_addr+off), &_w, 4); off += 4; } while (0)
  // x9 = &s_scroll_dbg_snap (homebrew RW memory)
  write_abs64(code_addr + off, 9, (uint64_t)&s_scroll_dbg_snap); off += 16;
  // Copy each field: ldr w10,[x0,#src] ; str w10,[x9,#dst]. x0 is only
  // ever read here (never a destination), so it's automatically still
  // the correct 'this' pointer when we fall through into Scroll() below.
  EMIT(ldr_w_imm(10, 0, 0x350)); EMIT(str_w_imm(10, 9, OFF_TX));
  EMIT(ldr_w_imm(10, 0, 0x354)); EMIT(str_w_imm(10, 9, OFF_TY));
  EMIT(ldr_w_imm(10, 0, 0x358)); EMIT(str_w_imm(10, 9, OFF_CX));
  EMIT(ldr_w_imm(10, 0, 0x35c)); EMIT(str_w_imm(10, 9, OFF_CY));
  EMIT(ldr_w_imm(10, 0, 0x368)); EMIT(str_w_imm(10, 9, OFF_FX));
  EMIT(ldr_w_imm(10, 0, 0x36c)); EMIT(str_w_imm(10, 9, OFF_FY));
  // planeW/planeH: direct fields on 'this', same shape as above.
  EMIT(ldr_w_imm(10, 0, 0x330)); EMIT(str_w_imm(10, 9, OFF_PLANEW));
  EMIT(ldr_w_imm(10, 0, 0x334)); EMIT(str_w_imm(10, 9, OFF_PLANEH));
  EMIT(ldr_w_imm(10, 9, OFF_SEQ));
  EMIT(add_w_imm(10, 10, 1));
  EMIT(str_w_imm(10, 9, OFF_SEQ));
  EMIT(0xd103c3ff); // sub sp, sp, #0xf0   re-execute the displaced instruction
  EMIT(encode_b(code_addr + off, resume_addr)); // b back into Scroll()
  #undef EMIT

  const PatchEntry e[1] = {
    { NULL, 0, ENTRY, 0xd103c3ff, encode_b(entry_addr, code_addr),
      "Scroll: entry hook -> scroll-debug snapshot cave (no calls, TEMPORARY)" },
  };
  apply_patches(mod, e, 1);
}

// Call once per real frame from main.c's loop (a context proven safe for
// debugPrintf all session). Only actually prints when the snapshot has
// genuinely new data (seq changed) AND at most a few times a second, so a
// 15-30s capture stays a readable size instead of one line per frame.
static void scroll_dbg_drain(void) {
  static uint32_t last_printed_seq = 0xFFFFFFFFu;
  static int frames_since_print = 0;
  frames_since_print++;
  uint32_t seq = s_scroll_dbg_snap.seq;
  if (seq == last_printed_seq) return;   // no new data since last print
  if (frames_since_print < 15) return;   // throttle to ~4x/sec at 60fps
  frames_since_print = 0;
  last_printed_seq = seq;
  extern Config config;
  debugPrintf("scroll dbg: tX=%.1f tY=%.1f cX=%.1f cY=%.1f fX=%.1f fY=%.1f "
              "planeW=%d planeH=%d zoom=%g\n",
              (double)s_scroll_dbg_snap.tX, (double)s_scroll_dbg_snap.tY,
              (double)s_scroll_dbg_snap.cX, (double)s_scroll_dbg_snap.cY,
              (double)s_scroll_dbg_snap.fX, (double)s_scroll_dbg_snap.fY,
              (int)s_scroll_dbg_snap.planeW, (int)s_scroll_dbg_snap.planeH,
              (double)config.field_zoom);
}

// ---------------------------------------------------------------------------
// apply_field_engine_registration_y -- DISABLED, round-7 post-mortem (not
// called anywhere -- kept for history like the other retired attempts).
//
// REVERTED after real-hardware testing, three independent findings:
//
//   1. ResetScrollAddress is NOT entry-only. The battle camera re-invokes
//      it while tracking enemy positions, and the fineY-seeding cave
//      (patch B) re-applied the 7-px phase on every call -- fighting
//      AutoScroll's own fine integration and producing constant vertical
//      shaking in battles. The "runs once per room entry" premise held for
//      field walking (all three static callers ARE entry/warp paths) but
//      battles reach it through a path the caller audit missed.
//
//   2. The target was wrong anyway: 210/zoom came from treating the
//      engine's internal 192-art window as what stock displays (112/192 =
//      58.3%). Stock actually DISPLAYS a 216-art window (viewH = 216,
//      y-scale 1.66667) -- the character sits at 112 * 1.66667 = 186.67
//      design px (~52%). So even where this patch "worked", the character
//      still sat visibly low, scaling with zoom.
//
//   3. The layer was wrong: patching the engine camera moves EVERYTHING
//      that consumes it (battle framing, scripted pans, edge clamping)
//      to chase a render-side registration goal. The render side already
//      has the correct, single, everything-stays-coherent lever: viewH,
//      whose formula in apply_field_view_zoom positions the visible
//      window outright (tY = camTopArt + viewH - planeH pins the visible
//      bottom at camTop + viewH). See apply_field_view_zoom's header for
//      the full corrected model. Scrolling was never the disease -- it
//      was the symptom.
//
// The mechanism analysis in the original header below (entry registration
// persists through DoScroll's lockstep pair-add; single shared ret; w9
// dead; edge branches untouched) remains disassembly-true and is kept for
// reference -- it was the premise and the target that were wrong, not the
// instruction-level work.
// ---------------------------------------------------------------------------
// [Original header follows]
//
// apply_field_engine_registration_y -- THE vertical centering fix, applied at
// the engine camera's ONE source of truth instead of any render-side copy.
//
// WHY THE CHARACTER SITS LOW AT ZOOM 2.0 (root cause, finally complete):
//
// The vertical registration is not set by FieldMap::Scroll, setScroll, the
// node position, or the render limits -- it is set ONCE per room entry by
// FieldImpl::ResetScrollAddress and then merely PRESERVED by everything else:
//
//   1. ResetScrollAddress(this, tileX, tileY) -- runs on every room entry /
//      warp (all three callers verified: FieldMap::setScroll 0x576d24, plus
//      0x5811bc and 0x58d15c, all via the PLT stub 0xb400a0) -- places the
//      camera window top at
//            camTop = 2*tileY - 14   [chips; 1 chip = 8 art px]
//      in the general mid-map case (0x576e98-0x576ea8), i.e. exactly 112
//      art px above the player's tile row. The near-top / near-bottom
//      branches clamp to the map edge instead. Its tail (0x576e70-0x576e7c)
//      zeroes the fine (sub-chip) offsets and returns through a single
//      shared ret at 0x576e80.
//
//   2. FieldImpl::DoScroll integrates the camera incrementally as the player
//      walks: fineY at state+0x1b4 (masked &0xf, chip-borrow tested &7 --
//      0x59d064/0x59d07c/0x59d114, so ANY initial phase 0..7 integrates
//      cleanly), and, decisively, its tail at 0x59d688-0x59d6b0 adds the
//      accumulated chip delta to camTop AND camBottom as a PAIR
//      (ldr d1,[x8,#0x18]; add v0.2s; str d0) -- it never re-derives one
//      from the other and never re-enforces the 24-chip height. The entry
//      registration therefore persists verbatim through all walking.
//
//   3. FieldMap::Scroll (0x5779fc-0x577a6c, this exact binary) converts that
//      state to the render target every frame:
//            camTopArt = camTop*8 + (fineY & 7)     [state+0x18 == the
//                                     "[x28,#8]" chip field; +0x1b4 == the
//                                     "[x28,#0x1a4]" fine field -- the two
//                                     struct bases differ by 0x10, verified
//                                     by three independent field matches]
//            tY (+0x354) = camTopArt + viewH - planeH
//      so the character's art-px offset from the view TOP is the constant
//      the engine chose: 112. On screen that is 112*zoom design px --
//      zoom-DEPENDENT: 210 at stock 1.875, 224 at 2.0 (14 px too low, the
//      exact symptom), 196 at 1.75 (14 px high, why 1.75 "looked centered").
//      Cross-checked against the real debug logs: tY = camTopArt+viewH-planeH
//      reproduces every logged value, and fX/fY = (-64, viewH-256) fall
//      straight out of the same disassembly with scroll cancelling -- also
//      matching all five logs bit-for-bit.
//
// THE FIX: keep the character at the STOCK screen registration (what you see
// with field_zoom_fix off) at every zoom, by making the entry gap
// zoom-aware:  gap = 210/zoom art px  (== 112 at 1.875, no-op by identity).
// Split into chips + sub-chip residual:  8*N - f = 210/zoom,  f in [0,7]:
//
//   zoom 1.875 -> N=14, f=0   (bit-identical stock, patch skips itself)
//   zoom 2.0   -> N=14, f=7   (chip constant UNCHANGED -- the entire fix is
//                              a 7-art-px fine phase; char rises 14 design px)
//   zoom 1.75  -> N=15, f=0   (char drops 14 design px to match stock)
//   zoom 3.0   -> N=9,  f=2
//
// Patch A (chip part): the single mid-map constant at 0x576e9c
// (sub w10,w9,#14 -> #N). The edge-clamped branches are NOT touched, so
// entry/clamp behavior at map edges stays byte-identical to today.
//
// Patch B (residual, only when f != 0): the shared ret at 0x576e80 branches
// to a 3-instruction cave that seeds fineY AFTER the tail has zeroed it:
//       movz w9, #f
//       str  w9, [x8, #0xc]     ; x8 = state+0x1a8 here (set at 0x576e74),
//                               ; so +0xc == state+0x1b4 == fineY
//       ret
// w9 is dead at the ret (last written at 0x576e24/0x576e30, consumed by
// 0x576e2c) and x8 is exactly state+0x1a8 on every path -- ALL branches of
// the function funnel through this one tail, so the phase is seeded on
// every entry type uniformly.
//
// WHY THIS CANNOT DESYNC PLANE FROM SPRITES (the failure that killed the
// direct Scroll()-tY attempt): the shift lands on the single upstream camera
// state that every consumer reads -- render tY, the tile-recycler window,
// the engine's own gating -- so it is indistinguishable from the camera
// legitimately standing 7 art px elsewhere, a state normal walking passes
// through constantly.
//
// WHY THIS IS NOT THE FAILED chips=14->12 EXPERIMENT (round 6): that patch
// also moved camBottom (+10 -> +12), changing the window's bottom extent
// that entry-time edge logic pairs with map bounds -- and it was tested only
// as a fixed 12, wrong at every zoom. This patch leaves camBottom stock, and
// DoScroll's verified pair-add means top and bottom are never re-derived
// from each other afterward, so the (harmless) height difference persists
// without anything re-normalizing it.
//
// EDGE MARGINS (checked, not assumed): the drawn plane window and the
// visible view are BOTH anchored to camTop, so a camTop bias moves the map
// under the view without changing view-vs-drawn-window alignment -- the
// class of black-bar bug from the node_y attempts is structurally impossible
// here. Bottom-clamped entries: at zoom 2.0 the view bottom moves from
// (map edge - 12 art) to (map edge - 5 art), still inside; at 1.75 the chip
// change moves today's +13.7-art overshoot (already absorbed fine by the
// zoom-aware render limits) to +5.7 -- strictly safer.
//
// KNOWN RESIDUALS to watch for on hardware, both cosmetic and bounded:
//   (a) DoScroll's special top snap (gated on map id 0x7f, 0x59d0b0-0x59d0cc)
//       zeroes fineY -- on that one map the 7-px residual is lost until the
//       next room entry (chip part unaffected).
//   (b) Scripted pans (atel_fscroll/atel_vfscroll write ABSOLUTE chip targets
//       to state+0x8/+0xc; AutoScroll converges camTop to them) frame at
//       script-authored positions -- unchanged from the current build. If a
//       post-cutscene "return to player" ever reseeds registration with
//       stock constants, it would revert until the next map entry; the
//       candidate site is CheckScroll's target-box writer at 0x5a1780
//       (the #0x70 constant). Not patched blind -- only if testing shows it.
// ---------------------------------------------------------------------------
__attribute__((unused))
static void apply_field_engine_registration_y(so_module *mod, float zoom) {
  if (zoom < 0.05f) zoom = 0.05f;  // guard: div-by-zero

  const float gap_art = 210.0f / zoom;      // required char->camTop gap, art px
  int n_chips = (int)(gap_art / 8.0f);      // floor...
  if ((float)n_chips * 8.0f < gap_art - 0.01f) n_chips++;   // ...to ceil
  int f_px = (int)((float)n_chips * 8.0f - gap_art + 0.5f); // residual, 0..7
  if (f_px < 0) f_px = 0;
  if (f_px > 7) f_px = 7;
  if (n_chips < 1) n_chips = 1;
  if (n_chips > 0xfff) n_chips = 0xfff;     // sub imm12 range

  const PatchEntry chip = { NULL, 0, 0x576e9c, 0x5100392a,
    0x51000000u | ((uint32_t)(n_chips & 0xFFF) << 10) | (9u << 5) | 10u,
    "ResetScrollAddress: mid-map camera-top gap 14 chips -> ceil((210/zoom)/8)" };
  apply_patches(mod, &chip, 1);

  if (f_px == 0)
    return;  // gap divisible by 8 (1.875, 1.75, ...): chips alone are exact

  const uint32_t CAVE     = 0xd047c;   // verified all-zero, R+X segment,
                                        // 12 bytes used, no other patch or
                                        // loader reference touches it
  const uint32_t RET_SITE = 0x576e80;  // the function's single shared ret

  uintptr_t base = (uintptr_t)mod->load_base;
  uintptr_t cave = base + CAVE;

  uint32_t off = 0;
  #define EMIT(w) do { uint32_t _w = (w); __builtin_memcpy((void*)(cave+off), &_w, 4); off += 4; } while (0)
  EMIT(movz_w(9, (uint16_t)f_px));  // w9 = f  (w9 verified dead at the ret)
  EMIT(str_w_imm(9, 8, 0xc));       // fineY (state+0x1b4) = f; x8 = state+0x1a8
  EMIT(0xd65f03c0);                 // ret
  #undef EMIT

  const PatchEntry hook = { NULL, 0, RET_SITE, 0xd65f03c0,
    encode_b(base + RET_SITE, cave),
    "ResetScrollAddress: ret -> fineY-residual cave (sub-chip camera phase)" };
  apply_patches(mod, &hook, 1);
}

// ---------------------------------------------------------------------------
// map_zoom_fix / map_zoom -- WorldMap (overworld) counterpart to
// field_zoom_fix / field_zoom.
//
// The identical hard-coded (1.875, 1.66667) setScale(x,y) pattern found in
// FieldMap::makeField also appears, via the same idiom (immediate-encoded
// fmov pair feeding vtable slot 0x90 -- Node::setScale), at four WorldMap
// sites -- confirmed by disassembling the actual shipped libchrono.so
// (aarch64-linux-gnu-objdump, cross-checked with a whole-binary scan for the
// `fmov s0,#1.875` immediate encoding, 0x1e2fd000 -- 7 hits total: one is
// FieldMap::makeField itself, already owned by apply_field_zoom above; two
// (AgeSelectScene::init 0x744264, SpecialEventScene::init 0x777ed0) are left
// alone, same as the file's existing policy for BattleMenu::Proc -- no
// evidence they're the same "map node" idiom rather than a coincidental
// reuse of the same ratio elsewhere; the remaining four are exactly the
// WorldMap sites below, and nothing else):
//
//   WorldMap::Init2          0x607c98   (first  fieldmap-alike node)
//   WorldMap::Init2          0x607db4   (second fieldmap-alike node)
//   WorldMap::exitMiniMap    0x609c28
//   WorldMap::initWeatherMap 0x60840c   (special -- see below)
//
// Each of the first three sites disassembles to the exact same 4-instruction
// idiom (BASE = the fmov-s0 address; verified byte-identical old words at
// all three):
//   BASE-8  movz w9, #0x5555             start building 1.66667 into w9
//   BASE-4  (unrelated -- a pointer load or "mov x0,x<n>" the surrounding
//            setScale call needs; scheduled into the gap by the compiler)
//   BASE    fmov s0, #1.875              single-instruction FP immediate
//   BASE+4  movk w9, #0x3fd5, lsl #16    finish building 1.66667 into w9
//   ...     fmov s1, w9                  elsewhere close by -- NOT patched,
//                                        see below for why it doesn't need
//                                        to be
//
// This is the SAME shape apply_field_zoom patches above -- except there the
// four instructions are contiguous (0x5761fc-0x576210), so w9 is fully built
// by the time BOTH fmov s0,w9 and fmov s1,w9 read it, and a full-precision
// movz+movk works. Here, s0's read (BASE) falls BETWEEN the two halves of
// the w9 build (BASE-8 and BASE+4), so at that point w9 only has its low 16
// bits set -- there is no instant where w9 is both complete and unread by
// s0. Building a second, independent value in another register would need
// two more instructions we don't have room for without a branch to a cave,
// which is more machinery than this warrants.
//
// Fix: build the FULL value in front of s0's read using the same one-
// instruction, top-16-bits-only encoding apply_field_view_zoom already uses
// for exactly this kind of squeeze (movz_topf: 7 mantissa bits -- exact for
// whole and most half/quarter-step zoom values, and nowhere near enough
// error at any value to be visible on a handful of screen pixels). That
// frees the movk slot entirely: with w9 already final one instruction
// earlier, the old movk would only serve to stomp its upper 16 bits back to
// the hardcoded 0x3fd5 (1.66667) before fmov s1 reads it, so it's nopped.
// fmov s1,w9 is left completely untouched at every site -- it already reads
// w9 by register, so it picks up the new value the instant its producer
// changes, with no need to know or touch its own address.
//
// The fourth site, WorldMap::initWeatherMap, uses the same building blocks
// but s0 isn't a single hardcoded constant there -- it's one of TWO values,
// chosen by an fcsel gated on w20==1 (disassembly, addresses absolute):
//   0x608404  mov   w8, #0x40160000        2.34375f -- already a
//                                           movz_topf-style single
//                                           instruction in stock (top16 of
//                                           2.34375f is exactly zero-padded)
//   0x608408  ldr   x9, [x23]              unrelated, untouched
//   0x60840c  fmov  s0, #1.875             default candidate
//   0x608410  fmov  s1, w8                 unrelated *yet* -- temp holds the
//                                          2.34375 candidate for fcsel below
//   0x608414  cmp   w20, #1                untouched
//   0x608418  mov   x0, x23                untouched
//   0x60841c  ldr   x8, [x9, #144]         untouched -- vtable slot fetch
//   0x608420  movz  w9, #0x5555            start building 1.66667
//   0x608424  movk  w9, #0x3fd5, lsl #16   finish building 1.66667
//   0x608428  fcsel s0, s1, s0, eq         s0 = (w20==1) ? 2.34375 : 1.875
//   0x60842c  fmov  s1, w9                 s1 = 1.66667, unconditionally
//   0x608430  blr   x8                     call setScale(this, s0, s1)
// 2.34375 / 1.875 = 1.25 exactly -- whatever this alternate state actually
// is (a weather overlay, most likely, given the function name; the gating
// condition itself wasn't decoded and isn't needed to be), it scales the
// map 25% larger on X only, with Y unaffected. That ratio is preserved
// rather than discarded: the default candidate becomes zoom, the alternate
// becomes zoom*1.25, and Y becomes zoom -- matching the three simple sites.
// The same one-instruction w9 build frees a slot here too, but instead of
// nopping it, that slot becomes a live "fmov s0,w9" landing immediately
// before fcsel reads s0 as its false-case operand -- valid because nothing
// else reads or writes s0 between the old fmov-s0 site and fcsel (verified:
// the intervening writes are to s1 and w9 only). fcsel and the trailing
// fmov s1,w9 are both untouched; each already reads a register, so both
// pick up the new contents automatically.
//
// Every old_word below was read directly out of this build's libchrono.so
// with aarch64-linux-gnu-objdump before writing any of this, the same way
// every other patch in this file is verified -- apply_patch's own
// mismatch check (see top of file) is the second, independent safety net:
// a build with different bytes at these addresses skips each entry and
// logs a warning instead of writing anything.
//
// STILL-OPEN, same spirit as field_zoom's own documented caveats: this
// patch touches ONLY the four setScale immediates above -- no position,
// anchor, or camera/scroll code, on WorldMap or anywhere else, is read or
// written by it. That means it cannot itself shift, mis-center, or misalign
// the map; the only thing map_zoom changes is how big the map is drawn
// (uniformly -- X and Y are always set equal), which changes how much of it
// fits in whatever fixed-size container/RenderTexture WorldMap composites
// into. That container's size has NOT been located or hardware-verified the
// way FieldMap's 432x224 RenderTexture floor was (see field_zoom's own
// comment above) -- so while the default (2.0, matching field_pixel_
// perfect's own original framing) is expected to be safe, values far from
// it are unverified and could reveal a similar floor/ceiling. Test on real
// hardware before shipping a non-default value.
// ---------------------------------------------------------------------------
static void apply_map_zoom(so_module *mod, float zoom) {
  if (zoom < 0.05f) zoom = 0.05f;  // guard: div-by-zero / degenerate scale

  // Round to nearest top-16 float before handing to movz_topf -- same
  // rounding apply_field_view_zoom uses for its own runtime-computed
  // values (movz_topf itself just truncates, which is fine for the fixed,
  // already-top16-exact literals it was originally written for, but zoom
  // is arbitrary user input here).
  #define RN16(f) ({ union { float _f; uint32_t _u; } _x; _x._f = (f); \
                     _x._u = (_x._u + 0x8000u) & 0xFFFF0000u; _x._f; })
  const float zoom_rn   = RN16(zoom);
  const float zoom_alt  = RN16(zoom * 1.25f);  // initWeatherMap's alt candidate
  #undef RN16

  const uint32_t movz9      = movz_topf(9, zoom_rn);
  const uint32_t fmov_s0_w9 = fmov_s_from_w(0, 9);
  const uint32_t NOP        = 0xd503201f;

  // -- The three simple sites --
  static const uint32_t SIMPLE_BASES[3] = {
    0x607c98,   // WorldMap::Init2, first fieldmap-alike node
    0x607db4,   // WorldMap::Init2, second fieldmap-alike node
    0x609c28,   // WorldMap::exitMiniMap
  };
  for (int i = 0; i < 3; i++) {
    const uint32_t base = SIMPLE_BASES[i];
    const PatchEntry e[3] = {
      { NULL, 0, base - 8, 0x528aaaa9, movz9,
        "WorldMap setScale: movz w9,#0x5555 -> movz w9,#topbits(zoom)" },
      { NULL, 0, base,     0x1e2fd000, fmov_s0_w9,
        "WorldMap setScale: fmov s0,#1.875 -> fmov s0,w9" },
      { NULL, 0, base + 4, 0x72a7faa9, NOP,
        "WorldMap setScale: movk w9,#0x3fd5,lsl#16 -> nop (w9 already final)" },
    };
    apply_patches(mod, e, 3);
  }

  // -- WorldMap::initWeatherMap (conditional X candidate) --
  {
    const uint32_t movz8_alt = movz_topf(8, zoom_alt);
    const PatchEntry e[4] = {
      { NULL, 0, 0x608404, 0x52a802c8, movz8_alt,
        "initWeatherMap: mov w8,#2.34375f -> mov w8,#topbits(zoom*1.25)" },
      { NULL, 0, 0x60840c, 0x1e2fd000, NOP,
        "initWeatherMap: fmov s0,#1.875 -> nop (dead write, replaced below)" },
      { NULL, 0, 0x608420, 0x528aaaa9, movz9,
        "initWeatherMap: movz w9,#0x5555 -> movz w9,#topbits(zoom)" },
      { NULL, 0, 0x608424, 0x72a7faa9, fmov_s0_w9,
        "initWeatherMap: movk w9,#0x3fd5,lsl#16 -> fmov s0,w9 (fcsel operand)" },
    };
    apply_patches(mod, e, 4);
  }
}

// ---------------------------------------------------------------------------
// apply_map_node_anchor -- the field_node_anchor counterpart for WorldMap.
// This is the piece config.h's map_zoom_fix comment and apply_map_zoom's
// own header both flag as missing ("does NOT come with a companion view-
// size/scroll-limit correction... cannot shift or mis-center the map [on its
// own]"). It closes that gap for the specific bug being reported: off-center
// framing at non-stock map_zoom values, the exact same symptom field_zoom
// had before apply_field_node_anchor existed.
//
// DISASSEMBLY (this build's libchrono.so, aarch64-linux-gnu-objdump,
// verified byte-for-byte via direct file read before writing any of this --
// same standard as every other patch here):
//
// WorldMap::Init2 contains the setScale call apply_map_zoom already patches
// at 0x607c98 ("first fieldmap-alike node"), immediately followed by its
// setPosition call at 0x607ce4 (vtable slot +200 -- the SAME setPosition
// slot FieldMap::makeField calls). Stock computes the two args as:
//
//   x9  = adrp/ldr &ctr::x_offset (0xbc6000+3240)   -- IDENTICAL global,
//         identical adrp+ldr idiom, to the one apply_field_node_anchor's
//         header already documents for FieldMap::makeField. Not a
//         coincidence: it's the same "half of a 480-wide art window,
//         centered in the current design width" helper value, reused here.
//   s0  (x-arg) = *x9                     = ctr::x_offset (80.0 under our
//                                            fixed 640-wide design canvas)
//   s1  (y-arg) = getVisibleSize().height + (-320.0)
//                                          = visibleH - 320.0 (40.0 under
//                                            our fixed 640x360 canvas, via
//                                            Director::getInstance()->
//                                            getVisibleSize(), confirmed by
//                                            resolving the two BL targets
//                                            against .rela.dyn: 0xb3f9c0 ->
//                                            cocos2d::Director::getInstance,
//                                            0xb3f9d0 -> ::getVisibleSize)
//
// Both args are blind to zoom, exactly like FieldMap's old (ctr::x_offset,
// 0.0) pair -- and for the same reason: this code predates map_zoom
// entirely, so nothing here was ever meant to track it. As zoom moves off
// the stock 1.875/1.66667 pair apply_map_zoom now lets you pick, the node
// grows/shrinks around this fixed anchor instead of around the design-
// canvas center, producing the reported off-center framing (worse the
// further zoom sits from stock, same shape of bug as field_zoom's original
// one).
//
// X FIX -- high confidence, not a new derivation: this is bit-for-bit the
// same mechanism apply_field_node_anchor already proved out (same global,
// same vtable slot, same "half art-window centered in design width" shape),
// so it gets the same formula:
//     node_x = 320.0 - 128.0*zoom
// (320 = half of the 640-wide design canvas from ui_scale_fix; 128
// = half of the shared engine's 256-art-px window -- see
// apply_field_node_anchor's header for the derivation of both constants.)
// At zoom=1.875 this reduces to exactly 80.0, bit-identical to stock
// ctr::x_offset under our fixed canvas -- no behavior change at the
// reference zoom, same identity guarantee every other zoom patch in this
// file gives.
//
// Y FIX -- ROUND 2 (superseding the round-1 top-anchor formula below).
// Round 1 deliberately rejected the naive "mirror the X formula" guess
// (180.0 - 96.0*zoom) because it doesn't reproduce the disassembled stock
// constant (40.0 at zoom=1.66667; the naive formula gives 20.0). That's
// correct as far as it goes -- solving backwards from the disassembled
// (visibleH - 320.0) formula does confirm stock anchors the map's TOP edge
// to the top of the design canvas and leaves the leftover 40.0-unit margin
// at the BOTTOM, not centered -- but round 1 stopped at "reproduce stock
// bit-for-bit" without asking whether stock's own framing is still the
// right TARGET once the rest of this mod has already changed the picture.
// It isn't, for two independent reasons, both confirmed against this
// exact config:
//
//   1. The bottom margin exists in stock ONLY to leave room for the
//      WorldMap MENU/MAP/WARP button row -- config.remove_mobile_ui (on by
//      default, and on in the reported config) already hides that row.
//      Preserving stock's reserved space for a UI element that's no longer
//      drawn just relocates dead space, it doesn't serve stock's original
//      purpose.
//   2. The top-anchor formula only reproduces that (now-pointless) framing
//      AT the single reference zoom (1.66667). Away from it -- including
//      at this config's own map_zoom=2.0 -- the content's own vertical
//      midpoint drifts: node_y + content_height/2 = (360-192*zoom) +
//      96*zoom = 360 - 96*zoom, which is 200 at zoom=1.66667 (already
//      20 above true center, i.e. never centered to begin with) but only
//      168 at zoom=2.0 -- 12 design-px BELOW the screen's true vertical
//      center (180), worse at higher zoom (72 at zoom=3.0, i.e. 108 low).
//      That's this bug: linear in zoom, and lines up exactly with "the
//      character sits a bit lower than center" reported at map_zoom=2.0.
//
// Fix: target the design canvas's actual vertical center instead of
// stock's UI-reserving offset -- back to the "obvious" mirror-of-X formula
// round 1 rejected, but now as the deliberate target rather than a
// rejected first draft:
//     node_y = 180.0 - 96.0*zoom
// (180 = half of the 640x360 design canvas; 96 = half of the shared
// engine's 192-art-px window, same source as X's 128 = half of 256.) This
// is an EXACT identity at every zoom, not just one reference value:
// node_y + content_height/2 = (180 - 96*zoom) + 96*zoom = 180, always --
// the content's own vertical midpoint sits at the design canvas's true
// center (180) regardless of zoom, by construction, the same guarantee
// the X formula already gives on its axis. (At zoom=1.66667 this now
// reads 20.0, not stock's 40.0 -- a deliberate, understood departure from
// stock, not a mistake: see point 1 above for why stock's own value is no
// longer the right target under this mod's own config.)
//
// Confidence note: same as round 1's, this is algebra, not a real-hardware
// result, and this project's history (apply_field_node_anchor's five-round
// Y saga) is a standing reminder that "the algebra checks out" and "it
// looks right on real hardware" are different claims. What round 2 adds
// over round 1 is a reason to believe it fixes the SPECIFIC reported
// symptom (low character at map_zoom=2.0, magnitude matches the -12
// computed here) rather than only an identity check at a zoom nothing here
// actually runs at. Still watch for the field_zoom failure signature (a
// solid-color or stale-content bar at the map's top or bottom edge) at
// zoom extremes (try 1.0 and 3.0 specifically) in case WorldMap has a
// render-target coupling like FieldMap's that disassembly alone doesn't
// surface -- the total blank area at any given zoom is unchanged from
// round 1 (still 360-192*zoom design-px of it when that's positive), only
// its top/bottom split moves from all-bottom to half-and-half, which is a
// smaller structural change than field_zoom's node-shift attempts made
// (those changed how much was shifted, not just where an existing gap
// sat) -- but it is still a change, not a no-op, so it isn't assumed safe.
// If a bar or corruption shows up, delete the y-arg patches below (keep
// the x-arg ones -- independent) and fall back to round 1's node_y =
// 360.0 - 192.0*zoom pending more data, same escalation path field used.
//
// NOT touched, and why: WorldMap::Init2's SECOND "fieldmap-alike" node
// (setScale at 0x607db4, already patched by apply_map_zoom) has its own
// setPosition call immediately after, but disassembly shows it does NOT
// share this node's formula at all -- x-arg is a flat compiled-in 328.0
// (no ctr::x_offset reference whatsoever), y-arg is (visibleH - 320.0) *
// 0.5, i.e. HALF of this node's y-shift. Different constants on both axes
// means this is very likely a structurally different layer (a border,
// shadow, or overlay plane at its own scale/position convention, not a
// second copy of the main map) -- guessing a formula for it from the outer
// shape alone, without knowing what it actually draws, is exactly the kind
// of blind patch this file's own history (see apply_field_camera_centering
// and the five-round apply_field_node_anchor saga) warns against. Left
// alone pending a screenshot/log-driven investigation of what that layer
// actually is, same standard as every other "not yet located" gap in this
// file. WorldMap::exitMiniMap (0x609c28) and WorldMap::initWeatherMap
// (0x60840c), the other two apply_map_zoom sites, were also checked end to
// end: neither is followed by ANY setPosition call on the scaled node (both
// fall straight into unrelated setVisible/cleanup code), so whatever
// position those nodes have was already set once elsewhere and isn't
// reset here -- nothing for this function to patch at either site.
//
// Mechanics: the same 10 instruction words between the setScale call and
// setPosition's blr (0x607cbc-0x607ce0) that stock uses to load ctr::
// x_offset and compute visibleH-320.0 are reused in place -- no cave, no
// branch, unlike apply_field_node_anchor (which only had 2 free slots and
// needed one). Here there are 7 free slots after removing the old x/y
// computation, room enough for the whole new computation (movz_topf +
// fmov, per axis, 4 instructions total) with 3 left over, NOPed. The 3
// instructions that resolve the vtable's setPosition slot and set x0=this
// (0x607cd0/0x607cd4/0x607cd8) are left completely untouched in their
// original positions -- verified not to depend on anything the removed
// instructions computed.
//
// ROUND 3 -- why round 2 alone still reads "a tad lower than center" on
// real hardware, and what config.map_zoom_y_trim is for.
//
// Round 2 is still correct as far as it goes: it is an exact identity for
// wherever the WHOLE composited WorldMap node ends up. But this session's
// disassembly (fresh read of this exact libchrono.so, not carried over
// from any earlier summary) found that the party-leader sprite is NOT
// positioned by this node at all -- it's positioned INSIDE a separate
// render target this node only displays after the fact, and that inner
// stage has its own, independent coordinate system:
//
//   - WorldObjectManager::Draw1/Draw2 (called from WorldImpl::drawWorld,
//     0x612060/0x61201c/0x6120cc) render every WorldObject sprite via a
//     direct cocos2d::Node::visit() call -- NOT via addChild, so these
//     sprites are never parented under the node apply_map_node_anchor
//     repositions. Draw1/Draw2 are always called between a matched
//     RenderTexture begin()/end() pair (vtable +0x530/+0x548) -- confirmed
//     by full disassembly of drawWorld -- so the sprites are baked into
//     that RT's own pixel buffer, at whatever coordinates they're given,
//     before the RT is ever composited onto the node this patch moves.
//   - That RT is NOT the four 512x512 tile-cache textures apply_map_zoom's
//     own header already accounted for. WorldMap::Init2 creates two
//     further RenderTextures at 0x220 x 0x100 (544x256 px, verified via
//     RenderTexture::create(w,h)'s own w0/w1 args immediately before each
//     call), stored at this+0x930 and this+0x938 -- the SAME two offsets
//     drawWorld's begin()/end() pair reads back every frame. This 544x256
//     size does not match either art-window constant (192 or 256) used
//     anywhere else in this file, so no existing formula in this file
//     already accounts for it.
//   - WorldObject::setPos (0x63ef8c, disassembled fresh this session)
//     computes the sprite's position inside that RT as a flat, zoom-blind
//     (x_arg + 192, 224 - y_arg) -- traced back through DrawSpriteAll
//     (0x63be84) to a per-object table at this+0x22d0 that the underlying
//     SNES-engine port (PutShape/HariCt/PartyMember, etc.) fills every
//     frame in native SNES OAM units. 224 is the SNES's own native active
//     picture height -- NOT the 192-art-px window apply_map_node_anchor's
//     own X/Y formulas are built from -- so there is no guarantee, and no
//     easy proof either way from static analysis alone, that this inner
//     224-tall (or 544x256-RT-tall) coordinate space's own effective
//     center lines up with the 192-tall reference the outer node math
//     targets. A mismatch of exactly this shape (an engine-internal
//     registration constant that doesn't match the render-side art window)
//     is the EXACT failure mode field_zoom's own multi-round Y saga above
//     turned out to be, right down to the "close, but a bit low, at every
//     zoom" symptom -- see apply_field_engine_registration_y's header for
//     the field-side version of this same class of bug.
//
// Fully closing this the way field_zoom eventually did (apply_field_
// engine_registration_y) needs either (a) tracing GetPartyCharPos/
// PutShape/HariCt's full OAM-table fill logic to get the party leader's
// exact native Y value on entry -- a large amount of additional SNES-
// engine-emulation disassembly with no guarantee of a clean answer, or
// (b) a hardware debug-log capture of that value directly (same tool this
// file already used for field_zoom -- see apply_field_scroll_debug_log
// above for the pattern). Neither is done yet.
//
// Until then, config.map_zoom_y_trim is the safe, immediate lever: a
// plain design-px additive term on top of round 2's node_y, defaulting to
// 0.0 (byte-identical to round 2 -- nothing changes until it's set). It
// rides the SAME already-X-proven node-position patch, so it cannot
// desync the background plane from anything else the way a wrong engine-
// level patch could -- it just slides the whole composited node (map +
// everything drawn into its RTs, sprites included) up or down as one
// unit. Positive values move the node UP the screen (this node's position
// is a bottom-left-anchor Y in cocos2d's Y-up convention, confirmed by
// the X-axis formula's own sign), which is the direction needed for
// today's reported symptom (character sits low -> raise it). Pick a
// value by eye in small steps (try +/-5, then +/-10, etc. design px) and
// rebuild; once it looks centered at your current config.map_zoom, note
// that the underlying inner-RT bug this works around is not itself proven
// zoom-proportional yet, so re-check by eye if you change map_zoom later.
// ---------------------------------------------------------------------------
static void apply_map_node_anchor(so_module *mod, float zoom) {
  if (zoom < 0.05f) zoom = 0.05f;  // guard: div-by-zero / degenerate anchor

  const float node_x = 320.0f - 128.0f * zoom;  // see header: high confidence
  const float node_y = 180.0f - 96.0f * zoom + config.map_zoom_y_trim;   // round 2 identity + round-3 empirical trim, see header

  const uint32_t movz_x = movz_topf(9, node_x);
  const uint32_t fmov_x = fmov_s_from_w(0, 9);
  const uint32_t movz_y = movz_topf(10, node_y);
  const uint32_t fmov_y = fmov_s_from_w(1, 10);
  const uint32_t NOP    = 0xd503201f;

  const PatchEntry e[7] = {
    { NULL, 0, 0x607cbc, 0x52b87408, movz_x,
      "WorldMap::Init2 map node setPosition: mov w8,#-320.0 -> movz w9,#topbits(node_x)" },
    { NULL, 0, 0x607cc0, 0xbd409fe0, fmov_x,
      "WorldMap::Init2 map node setPosition: ldr s0,[sp,#156] (visH) -> fmov s0,w9 (node_x)" },
    { NULL, 0, 0x607cc4, 0xf0002de9, movz_y,
      "WorldMap::Init2 map node setPosition: adrp x9,&ctr::x_offset -> movz w10,#topbits(node_y)" },
    { NULL, 0, 0x607cc8, 0x1e270101, fmov_y,
      "WorldMap::Init2 map node setPosition: fmov s1,w8(-320.0) -> fmov s1,w10 (node_y)" },
    { NULL, 0, 0x607ccc, 0xf9465529, NOP,
      "WorldMap::Init2 map node setPosition: ldr x9,[x9,#3240] (ctr::x_offset) -> nop" },
    { NULL, 0, 0x607cdc, 0x1e212801, NOP,
      "WorldMap::Init2 map node setPosition: fadd s1,s0,s1 (visH-320) -> nop" },
    { NULL, 0, 0x607ce0, 0xbd400120, NOP,
      "WorldMap::Init2 map node setPosition: ldr s0,[x9] (ctr::x_offset value) -> nop" },
    // 0x607cd0 ldr x8,[x22]     (vtable ptr)      -- UNCHANGED
    // 0x607cd4 mov x0,x22       (this)            -- UNCHANGED
    // 0x607cd8 ldr x8,[x8,#200] (setPosition slot) -- UNCHANGED
    // 0x607ce4 blr x8                              -- UNCHANGED
  };
  apply_patches(mod, e, 7);
}
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The bilinear patches use pattern-search within the function body (as the
// Python script does) rather than hard-coded relative offsets, because the
// compiler may arrange the instructions differently. We provide a dedicated
// helper that scans the known function range.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 10.  menu_alignment_fix
//
//     Equipment menu vertical text alignment + equip/stat icon centering
//     (A-F), plus Techs-page sub-chara panel alignment (G).
//
//     SYMPTOM (720p measurements, cap-glyph top/bottom rows from real
//     screenshots): the item-select list labels (i.e. "Wooden Sword", its qty "1")
//     span rows 247-261; the stat column (i.e. "Strength : 9" .. "Magic Defense :
//     6"), the category-button captions (i.e. "Bronze Blade" etc.), and the
//     equipped-item info column (i.e. the far right numbers and "Speed +1")
//     span 244-258 on its row -- a uniform 3-screen-px (= 1.5 design-unit at
//     the 2x handheld scale) HIGH offset. The HP/MP/LV/EXP/Next block above
//     "Strength" is NOT part of this -- it was never misaligned against
//     anything else on the page and must stay exactly where stock puts it.
//
//     ROOT CAUSE (from disassembly of the two label pipelines):
//       * openItemSelectNode (0x70b6d8) -- the only correct one -- places
//         its labels ANCHOR_MIDDLE_LEFT at y = buttonHeight * 0.5, i.e.
//         truly vertically centred whatever the label box height is.
//       * The stat/button/info-column labels are TOP-anchored with
//         hand-tuned offsets that assume a 16-unit label box: top-anchored
//         text matches middle-anchored text only when
//         (buttonHeight/2 + labelH/2) == chosen top offset, and the
//         offsets used (e.g. y = buttonHeight - 10 in
//         createCategoryButtons) solve that for labelH = 16.
//     Since the item list is both correct-by-construction and the page's
//     dominant interactive element, the stat rows and everything keyed off
//     them move DOWN 1.5 to match it. The top block is left untouched.
//
//     FIVE placement paths cover 100% of the page's text and its portrait
//     (verified by scanning every MenuNodeEquip function for createLabel /
//     drawIcon / drawItemMsgSub / ParameterLabel::create /
//     createWithSpriteFrameName call sites -- there are no others), each
//     fixed by the mechanism that fits its code shape:
//
//     A. MenuNodeEquip::setupPageFixedLabels (0x708948) -- 35 static
//        entries total: the 7 stat-name/colon/value-column rows
//        ("Strength" @ y=-106 down through "Magic Defense" @ y=-232, step
//        21) move DOWN 1.5 to match the item list (see below); the
//        HP/MP/LV/EXP/Next block and its weapon/shield icons (y=-44..-76)
//        move UP 1.0 instead, to track the portrait fix in section E. The
//        35 Y constants are materialised six different ways (rodata Vec2
//        literals, packed MOVZ+MOVK pairs, lone MOVZ floats), so per-site
//        immediate edits can't discriminate the groups cheaply -- instead
//        the entry-emission loop head is hooked once, and the cave itself
//        branches on the value: loop at 0x709b40..0x709bac walks the
//        0x38-byte stack entries ({type, Vec2 pos, string, anchor, icon})
//        dispatching to drawIcon (type 1) / createLabel (type 0); head
//        instruction 0x709b70 `ldr w8,[x21,x20]` (both loop-entry paths
//        land here) is displaced into a cave that reads entry->pos.y,
//        compares its raw bits (unsigned) against the bit pattern for
//        -90.0f -- a clean midpoint between the top block's shallowest row
//        (-76) and the stats' deepest (-106), safe because IEEE-754 bit
//        patterns of same-sign floats preserve magnitude ordering under
//        unsigned compare -- and applies -1.5 at or past the stats band
//        (y <= -90, bits >= 0xC2B40000) or +1.0 above it (top block).
//        x8/x9/s0/s1 are all dead/call-clobbered at the hook point and
//        carry nothing across the loop; entries rebuild on the stack every
//        call, so neither adjustment can accumulate.
//        Cave: 0xaa7b30, a 96-byte verified-zero inter-function padding
//        run in the same padding neighbourhood as the two proven caves at
//        0xaa56a0/0xaa6270 (both re-checked: no overlap, and this loader
//        never reads that region). The top-block branch also carries
//        section F2's x >= 240 -> +12.0 column shift. Uses 92 of the 96
//        bytes.
//
//     B. MenuNodeEquip::setupPageParameterLabels (0x70a110) -- ONLY the
//        seven live-updating stat values at x=132 (Strength..Magic
//        Defense) move; the three top-block values (HP-row @ (236,-44),
//        weapon-row @ (236,-60), defense-row @ (332,-60)) are deliberately
//        left unpatched -- the pristine .so already has them exactly
//        where they belong. Each moved literal is single-xref (confirmed
//        by a binary-wide adrp+ldr scan), so the Y float is patched in
//        place -- a data patch, so the 0.5 fractions are exact.
//
//     C. MenuNodeEquip::createCategoryButtons (0x70ae10) -- button caption
//        + in-button item icon both derive y from s11 = buttonHeight + s10
//        with s10 loaded once by `fmov s10,#-10.0` (only use of s10 in the
//        function); -11.5 is FMOV-imm8 encodable, so one word changes and
//        caption + icon move DOWN together, preserving their relative
//        alignment to each other. That still leaves the icon's own visual
//        centre sitting low against the caption's cap-height (measured
//        avg +1.75 design-units low across the three unfocused buttons:
//        Bronze Helm/Hide Tunic/Headband, 2.5/4.5/3.5 screen-px at 2x) --
//        drawIcon (0x734a38) anchors TOP_LEFT, so its sprite's own
//        vertical mass sits differently than a text cap-height box.
//        Fixed with a second, icon-only hook at the icon's position store
//        0x70b170 `str s11,[sp,#0x34]` (this exact stack slot is reused a
//        few instructions later for the caption's own y -- unaffected,
//        since the cave never writes to s11 itself, only to scratch s9).
//        Cave @ 0xaa3db0 (80B verified-zero, s9 confirmed dead in-loop --
//        its only two reads are both before the loop starts) computes
//        s9 = s11 + 1.75 and stores that instead, for the icon only.
//
//     D. MenuNodeEquip::UpdateEquippedItemDescription (0x70a348) -- the
//        per-category info column at x=310: container y = -110 - 42*cat
//        (integer, via scvtf, so it can't take the .5 alone), value label
//        at rodata (34,4) TOP_RIGHT, "Speed +1" msg-sub at rodata (40,4)
//        (both literals single-xref, same scan as B). Text shift is split:
//        container -110 -> -111 (-1.0) and both label literals 4.0 -> 3.5
//        (-0.5), netting exactly -1.5 for the digit/message text. The
//        little stat icon (dagger/shield, drawn via drawIcon at the
//        container's local origin (0,0)) rides only the container's -1.0
//        and was measured sitting a further ~3.8 design-units low against
//        the digit's cap-height (avg of 8.0/7.5/7.5 screen-px at 2x across
//        the three visible rows) -- same TOP_LEFT-anchor-vs-cap-height
//        mismatch as (C). Fixed with a hook at the icon-position write
//        0x70a42c `str xzr,[sp,#8]` (originally zeroing the whole Vec2):
//        cave @ 0xaa4af0 (80B verified-zero) writes a packed
//        Vec2(0.0, 3.75) instead (single MOVZ, since only one 16-bit
//        chunk of the 64-bit pattern is non-zero), moving the icon up
//        3.75 units local to the already-shifted container -- x9 is dead
//        here (caller-saved, and about to be clobbered by the drawIcon
//        call regardless).
//
//     Icon-centring deltas (C: 1.75, D: 3.75) are measured/averaged from
//     screenshots against irregular sprite art rather than derived from an
//     exact geometric formula like the text fix was, so treat them as a
//     strong first pass -- nudge the two literal constants in the cave
//     bodies below (fmov s9,#1.75 / the 3.75 in the Vec2 pack) if hardware
//     testing shows either icon family needs a touch more or less.
//
//     E. Portrait/top-row follow-up (added after real-hardware testing of
//        A-D showed the character portrait sits a hair down-and-right on
//        the Equip page versus the main menu list): the portrait moves
//        UP+LEFT 1.0 unit each (2 screen-px at 2x) to cancel that offset
//        outright, while the rest of its row (name, "LV", the "AttrIcon"
//        element badge, and -- via section A's cave -- HP/MP/EXP/Next and
//        the weapon/shield icons) moves UP 1.0 only, staying anchored to
//        the portrait horizontally. See section E's own header, just above
//        its patch entries below, for the exact site-by-site breakdown.
//
//     F. Top-block horizontal finalisation (added after E's hardware pass):
//        F1 -- the element badge, character name, and "LV :  4" group move
//        LEFT 1.0 unit to finish tracking the portrait's E correction
//        (portrait itself already correct, HP/attack/EXP chunk stays);
//        F2 -- the MP/defense/Next column right-aligns its value boxes to
//        x=344, the equip-info "7"/"8"/"5" right edge, opening a small
//        deliberate gap between the two top-block chunks. In-loop pieces
//        ride a new x >= 240 branch in section A's cave; the out-of-loop
//        pieces (rodata literals, one sprite immediate) are direct edits.
//        See section F's own header above its entries for the breakdown.
//
//     NOT touched, deliberately: the stats rows below "Strength" (section
//     A's own -1.5 branch, unaffected by E/F), the bottom description bar
//     and "Equipment" tab (shared with every other menu), the item-select
//     list itself (the reference), the pager dots, arrows, and scrollbar.
//
//     All old words byte-verified against this exact libchrono.so.
//     Boot-time patch; menus are rebuilt per entry so no state carries.
// ---------------------------------------------------------------------------
static const PatchEntry g_text_alignment_patches[] = {

  // --- A. setupPageFixedLabels: -1.5 on stat rows (y <= -90), +1.0 on the
  //        top block (y > -90) -- both via one gated loop hook (see E).
  P_RAW(0x709b70, 0xb8746aa8, 0x140e77f0,
    "setupPageFixedLabels loop head ldr w8,[x21,x20] -> b 0xaa7b30 (gated y-shift cave)"),

  // Cave @ 0xaa7b30 (96B zero padding; entry 0x709b70 -> 0xaa7b30). Now a
  // TWO-WAY split instead of a skip: b.lo -> 0xaa7b5c (top block: +1.0,
  // see section E header below for why), fallthrough -> stats band -1.5
  // as before; both converge on the displaced load at 0xaa7b54 -> tail
  // b -> 0x709b74 (resume loop). Uses 60 of 96 bytes.
  P_CAVE(0xaa7b30, 0x8b1402a8, "cave: add x8,x21,x20 (entry ptr)"),
  P_CAVE(0xaa7b34, 0xbd400900, "cave: ldr s0,[x8,#8] (pos.y)"),
  P_CAVE(0xaa7b38, 0x1e260009, "cave: fmov w9,s0 (raw bits of y)"),
  P_CAVE(0xaa7b3c, 0x52b8568a, "cave: mov w10,#0xc2b40000 (-90.0f threshold)"),
  P_CAVE(0xaa7b40, 0x6b0a013f, "cave: cmp w9,w10 (unsigned: bigger bits = more negative = lower row)"),
  P_CAVE(0xaa7b44, 0x540000c3, "cave: b.lo 0xaa7b5c (top block: +1.0 routine, was a skip)"),
  P_CAVE(0xaa7b48, 0x1e3f1001, "cave: fmov s1,#-1.5"),
  P_CAVE(0xaa7b4c, 0x1e212800, "cave: fadd s0,s0,s1"),
  P_CAVE(0xaa7b50, 0xbd000900, "cave: str s0,[x8,#8]"),
  P_CAVE(0xaa7b54, 0xb8746aa8, "cave: ldr w8,[x21,x20] (displaced original; both paths land here)"),
  P_CAVE(0xaa7b58, 0x17f18807, "cave: b 0x709b74 (resume loop)"),
  // Top-block routine (E): +1.0 (2 screen-px at 2x -- see section E) on
  // the name/LV/HP/MP/EXP/Next row and its weapon/shield icons. Then (F)
  // the same entry's X is tested: entries at x >= 240 -- exactly the
  // MP/defense/Next column (its labels @248, colons @284, the "14/ 14"
  // slash @314, values @332, and the shield icon @248; the nearest
  // left-chunk piece is the EXP value's x=236 box and nothing sits in
  // 237..247) -- get +12.0 so the column's TOP_RIGHT value boxes land at
  // x=344, the same right edge as the equip-info "7"/"8"/"5" digits
  // (container x=310 + local 34). x < 240 (name/LV/HP/attack/EXP chunk)
  // falls through unmoved. Both exits rejoin the displaced load above.
  // x8/s0 still live from the common prologue; w9/w10/s1 are free (their
  // uses above are on the other path / already consumed). Positive-float
  // unsigned bit compare is magnitude-ordered, same trick as the -90 test.
  P_CAVE(0xaa7b5c, 0x1e2e1001, "cave: fmov s1,#1.0"),
  P_CAVE(0xaa7b60, 0x1e212800, "cave: fadd s0,s0,s1"),
  P_CAVE(0xaa7b64, 0xbd000900, "cave: str s0,[x8,#8]"),
  P_CAVE(0xaa7b68, 0xbd400500, "cave: ldr s0,[x8,#4] (pos.x; was the join branch, now falls through F)"),
  P_CAVE(0xaa7b6c, 0x1e260009, "cave: fmov w9,s0 (raw bits of x)"),
  P_CAVE(0xaa7b70, 0x52a86e0a, "cave: mov w10,#0x43700000 (240.0f threshold)"),
  P_CAVE(0xaa7b74, 0x6b0a013f, "cave: cmp w9,w10"),
  P_CAVE(0xaa7b78, 0x54fffee3, "cave: b.lo 0xaa7b54 (x < 240: left chunk stays; join tail)"),
  P_CAVE(0xaa7b7c, 0x1e251001, "cave: fmov s1,#12.0 (MP/def/Next column right-shift)"),
  P_CAVE(0xaa7b80, 0x1e212800, "cave: fadd s0,s0,s1"),
  P_CAVE(0xaa7b84, 0xbd000500, "cave: str s0,[x8,#4]"),
  P_CAVE(0xaa7b88, 0x17fffff3, "cave: b 0xaa7b54 (join displaced-load+resume tail)"),

  // --- B. setupPageParameterLabels: rodata Vec2 Y halves, STAT ROWS ONLY
  //        (the 3 top-block values are intentionally absent from this
  //        list -- the pristine binary already has them right).
  P_RAW(0x36198c, 0xc2d40000, 0xc2d70000, "param Vec2 (132,-106) y -> -107.5 (Strength value)"),
  P_RAW(0x361244, 0xc2fe0000, 0xc3008000, "param Vec2 (132,-127) y -> -128.5 (Accuracy value)"),
  P_RAW(0x36116c, 0xc3140000, 0xc3158000, "param Vec2 (132,-148) y -> -149.5 (Speed value)"),
  P_RAW(0x361314, 0xc3290000, 0xc32a8000, "param Vec2 (132,-169) y -> -170.5 (Magic value)"),
  P_RAW(0x361504, 0xc33e0000, 0xc33f8000, "param Vec2 (132,-190) y -> -191.5 (Evasion value)"),
  P_RAW(0x361754, 0xc3530000, 0xc3548000, "param Vec2 (132,-211) y -> -212.5 (Stamina value)"),
  P_RAW(0x3613a4, 0xc3680000, 0xc3698000, "param Vec2 (132,-232) y -> -233.5 (Magic Def value)"),

  // --- C. createCategoryButtons: caption+icon drop 1.5 together, then the
  //        icon gets an additional solo +1.75 to centre on the caption.
  P_RAW(0x70ae7c, 0x1e34900a, 0x1e34f00a,
    "createCategoryButtons fmov s10,#-10.0 -> #-11.5 (caption+icon drop 1.5)"),

  P_RAW(0x70b170, 0xbd0037eb, 0x140e6310,
    "createCategoryButtons icon y store str s11,[sp,#0x34] -> b 0xaa3db0 (icon-only +1.75 cave)"),

  // Cave @ 0xaa3db0 (80B verified-zero; entry 0x70b170 -> 0xaa3db0, tail
  // b -> 0x70b174 round-trip-verified). s9 confirmed dead at this point
  // (its only two reads are both before the per-button loop begins).
  P_CAVE(0xaa3db0, 0x1e2f9009, "cave: fmov s9,#1.75 (icon centring offset)"),
  P_CAVE(0xaa3db4, 0x1e2b2929, "cave: fadd s9,s9,s11 (icon y = caption y + 1.75)"),
  P_CAVE(0xaa3db8, 0xbd0037e9, "cave: str s9,[sp,#0x34] (icon only; caption's later write of s11 is untouched)"),
  P_CAVE(0xaa3dbc, 0x17f19cee, "cave: b 0x70b174 (resume)"),

  // --- D. UpdateEquippedItemDescription: container -1.0, labels -0.5 (net
  //        -1.5 for text), then the stat icon gets a solo local +3.75 to
  //        centre on the digit/message.
  P_RAW(0x70a394, 0x12800da9, 0x12800dc9,
    "equip-info container base mov w9,#-110 -> #-111"),
  P_RAW(0x36192c, 0x40800000, 0x40600000, "equip-info value Vec2 (34,4) y -> 3.5"),
  P_RAW(0x36150c, 0x40800000, 0x40600000, "equip-info msg-sub Vec2 (40,4) y -> 3.5"),

  P_RAW(0x70a42c, 0xf90007ff, 0x140e69b1,
    "equip-info icon pos store str xzr,[sp,#8] -> b 0xaa4af0 (icon-only +3.75 cave)"),

  // Cave @ 0xaa4af0 (80B verified-zero; entry 0x70a42c -> 0xaa4af0, tail
  // b -> 0x70a430 round-trip-verified). x9 dead here (caller-saved, about
  // to be clobbered by the drawIcon call regardless).
  P_CAVE(0xaa4af0, 0xd2e80e09, "cave: mov x9,#0x4070000000000000 (packed Vec2(0.0,3.75))"),
  P_CAVE(0xaa4af4, 0xf90007e9, "cave: str x9,[sp,#8] (icon local pos, was (0,0))"),
  P_CAVE(0xaa4af8, 0x17f1964e, "cave: b 0x70a430 (resume)"),

  // --- E. setupPage's portrait + attribute-icon sprites, and the top-block
  //        ParameterLabel trio: the last pieces of the equip page not
  //        covered by A-D.
  //
  //        SYMPTOM: entering Equip visibly nudges the character portrait
  //        down-and-right a hair versus its position on the main menu list
  //        (2 screen-px at 2x handheld = 1.0 design-unit, each axis).
  //        The rest of that same row (name, "LV", HP/MP, EXP/Next, and the
  //        weapon/shield icons) isn't being compared against anything
  //        off-page, but the user wants it visually anchored to the
  //        portrait rather than left behind when the portrait moves --
  //        so it moves UP the same 1.0 unit as the portrait, but NOT left
  //        (only the portrait needs the horizontal correction, since nothing
  //        else in that row is being asked to track the main-menu list).
  //
  //        Two sprites are created directly in setupPage (0x7084b0) itself,
  //        outside the 35-entry loop the section-A cave gates on, so they
  //        need their own direct edits:
  //          - portrait (createWithSpriteFrameName from the character's own
  //            param data) @ (12,-44) TOP_LEFT -> (11,-43): X -1.0 (left),
  //            Y +1.0 (up). X is FMOV-imm8 encodable directly (11.0 =
  //            (1+6/16)*2^3, same family as the original 12.0); Y goes
  //            through the existing mov-bits+fmov-reg pattern already used
  //            for -44.0, so only the immediate word changes.
  //          - the small "menu/AttrIcon%d" element/attribute badge @
  //            (68,-44) TOP_LEFT -> (68,-43): Y +1.0 only, matching the
  //            "icons move up, not left" rule; its X-bits word is untouched.
  //        Everything else in the row (the name/LV text and the weapon/
  //        shield icons drawn per-entry) is part of the section-A 35-entry
  //        loop and rides the cave's new top-block +1.0 branch above --
  //        no separate edit needed here.
  //
  //        The three top-block ParameterLabel values EXCLUDED by section B
  //        (HP-row (236,-44), weapon-row (236,-60), defense-row (332,-60))
  //        get the same +1.0: still single-xref rodata (same scan as B),
  //        so the Y float is patched in place, X untouched.
  P_RAW(0x70864c, 0x1e251000, 0x1e24d000, "portrait sprite fmov s0,#12.0 -> #11.0 (X, left 1.0)"),
  P_RAW(0x708648, 0x52b84609, 0x52b84589, "portrait sprite mov w9,Y-bits -44.0 -> -43.0 (up 1.0)"),
  P_RAW(0x708710, 0x52b8460a, 0x52b8458a, "AttrIcon sprite mov w10,Y-bits -44.0 -> -43.0 (up 1.0 only)"),
  P_RAW(0x3616c4, 0xc2300000, 0xc22c0000, "param Vec2 (236,-44) y -> -43.0 (HP-row value)"),
  P_RAW(0x361abc, 0xc2700000, 0xc26c0000, "param Vec2 (236,-60) y -> -59.0 (weapon-row value)"),
  P_RAW(0x361104, 0xc2700000, 0xc26c0000, "param Vec2 (332,-60) y -> -59.0 (defense-row value)"),

  // --- F. Top-block horizontal finalisation (two independent moves):
  //
  //     F1: the element/attribute badge, the character name, and the
  //         "LV :    4" group move LEFT 1.0 unit (2 screen-px at 2x),
  //         completing the up-left correction E started (E already moved
  //         them UP 1.0; only the portrait needed left then -- now this
  //         group follows it left too, per hardware review). The portrait
  //         itself and everything from HP rightward are NOT in this set.
  //         The name/LV/colon/value labels are the only fixed entries
  //         whose positions load from rodata Vec2 literals (all
  //         single-function-xref by the same binary-wide adrp+ldr scan as
  //         B; the LV-value literal's two sites are both in
  //         setupPageFixedLabels, one per language path), so their X
  //         floats are patched in place; the AttrIcon badge is a setupPage
  //         sprite whose X is its own MOVZ immediate.
  //
  //     F2: the MP/defense/Next column right-aligns to the equip-info
  //         stat digits: its in-loop pieces ride the section-A cave's new
  //         x >= 240 -> +12.0 branch (see the cave listing above), and the
  //         one out-of-loop piece -- the live defense value, a
  //         ParameterLabel at rodata (332,-60) -- gets the same +12 here:
  //         x 332 -> 344, the exact TOP_RIGHT box edge of the "7"/"8"/"5"
  //         (equip-info container x=310 + label local x=34). Same anchor
  //         and font on both, so the right edges land identically.
  P_RAW(0x361a20, 0x42880000, 0x42860000, "name Vec2 (68,-62) x -> 67.0 (left 1.0)"),
  P_RAW(0x3615b8, 0x42880000, 0x42860000, "LV label Vec2 (68,-76) x -> 67.0 (left 1.0)"),
  P_RAW(0x361238, 0x42ac0000, 0x42aa0000, "LV colon Vec2 (86,-76) x -> 85.0 (left 1.0)"),
  P_RAW(0x3617d0, 0x42f80000, 0x42f60000, "LV value Vec2 (124,-76) x -> 123.0 (left 1.0; both language paths)"),
  P_RAW(0x70870c, 0x52a85109, 0x52a850c9, "AttrIcon sprite mov w9,X-bits 68.0 -> 67.0 (left 1.0)"),
  P_RAW(0x361100, 0x43a60000, 0x43ac0000, "param Vec2 (332,-60) x -> 344.0 (defense value right edge = 7/8/5 edge)"),

  // --- G. Techs-page sub-chara panel (MenuNodeTech::setupSubCharas @0x722e94
  //        / nsMenu::createSubCharaStatusFrame @0x73635c):
  //
  //        SYMPTOM (720p measurements, both visible party rows): the
  //        left-panel character text (name, "LV :", "HP :", "MP :" and
  //        their values) sits a uniform 12 screen-px (6.0 design-units at
  //        2x) LEFT of the main-menu status cards' columns -- label left
  //        edges x=282 vs the cards' 294, value right edges 423 vs 435,
  //        identical on every row -- so the text visibly jumps sideways on
  //        the main-menu <-> Techs transition. Vertical placement already
  //        matches (name/LV/HP/MP cap bands land on the same rows in both
  //        screens) and needs nothing. The walking sprite is separately
  //        off-centre in its gutter: node centre at x=236 (frame-quad
  //        edges 219.5/252.5; = 80 canvas + 26 row + 12 local = design
  //        118), versus the panel-edge/text-column midpoint
  //        (200.5 + 293.5)/2 = 247 = design 123.5, i.e. 5.5 units short.
  //
  //        LAYOUT CHAIN (from disassembly): setupSubCharas places up to
  //        four row layers via packed-Vec2 setPosition calls at
  //        (26, -59/-143/-227/-311); the X half is materialised per row
  //        as `mov x8,#0x41d00000` (26.0f in the packed low word). Each
  //        row's children come from createSubCharaStatusFrame: sprite at
  //        local X = 12.0 (`fmov s0,#12.0`, the vtable+0xc8 position
  //        call), name label at rodata Vec2 (35,16) @0x361990, LV/HP/MP
  //        block at base Vec2 (35, y) (MOVZ 0x420c immediate @0x736524).
  //        80+26+35 = 141 = screen 282 reproduces the measurement exactly.
  //
  //        FIX -- decomposed to keep the shared text constants pristine:
  //        createSubCharaStatusFrame also serves MenuNodeFormation::setup
  //        (0x71013c) and MenuNodeTop::setup (0x7266bc) (both
  //        showName=false -- the name-label rodata never renders there,
  //        and it's single-xref @0x7364cc by the same binary-wide
  //        adrp+ldr scan as B, so it was patchable -- but any edit to the
  //        35.0 text constants would leak 1 screen-px of *column* drift
  //        into those screens, the very artifact this section exists to
  //        remove). Instead the entire +6.0 rides the four row-layer X
  //        immediates (26 -> 32), which are Techs-only code, landing the
  //        text exactly on the cards' columns; the sprite then takes
  //        local 12.0 -> 11.5 (FMOV-imm8 encodable, same edit shape as
  //        E's portrait fmov) for a net +5.5 = dead centre at 123.5. The
  //        half-unit sprite nudge is the only edit visible to
  //        Formation/Top -- their walking sprites shift 1 screen-px left,
  //        imperceptible on irregular sprite art.
  P_RAW(0x722fa0, 0xd2a83a08, 0xd2a84008, "tech row1 layer mov x8,X-bits 26.0 -> 32.0 (right 6.0)"),
  P_RAW(0x722fe8, 0xd2a83a08, 0xd2a84008, "tech row2 layer mov x8,X-bits 26.0 -> 32.0 (right 6.0)"),
  P_RAW(0x723030, 0xd2a83a08, 0xd2a84008, "tech row3 layer mov x8,X-bits 26.0 -> 32.0 (right 6.0)"),
  P_RAW(0x723078, 0xd2a83a08, 0xd2a84008, "tech row4 layer mov x8,X-bits 26.0 -> 32.0 (right 6.0)"),
  P_RAW(0x736458, 0x1e251000, 0x1e24f000, "sub-chara sprite fmov s0,#12.0 -> #11.5 (net +5.5, centred in gutter)"),
};

static void apply_bilinear_patches(so_module *mod) {
  // Locate the two target functions in the symbol table.
  uintptr_t iwm_base = 0, iwm_size = 0;  // initWithMipmaps
  uintptr_t aap_base = 0, aap_size = 0;  // setAntiAliasTexParameters

  for (int i = 0; i < mod->num_syms; i++) {
    const char *nm = mod->dynstrtab + mod->syms[i].st_name;
    if (__builtin_strncmp(nm, "_ZN7cocos2d9Texture2D15initWithMipmaps",
                          sizeof("_ZN7cocos2d9Texture2D15initWithMipmaps") - 1) == 0) {
      iwm_base = (uintptr_t)mod->load_base + mod->syms[i].st_value;
      iwm_size = mod->syms[i].st_size;
    } else if (__builtin_strncmp(nm, "_ZN7cocos2d9Texture2D25setAntiAliasTexParameters",
                                 sizeof("_ZN7cocos2d9Texture2D25setAntiAliasTexParameters") - 1) == 0) {
      aap_base = (uintptr_t)mod->load_base + mod->syms[i].st_value;
      aap_size = mod->syms[i].st_size;
    }
  }

  if (!iwm_base || !aap_base) {
    debugPrintf("patches: bilinear -- Texture2D symbols not found (different build?)\n");
    return;
  }

  // Pattern-search and patch table: (old, new, desc, base, size)
  // Each entry is searched within the relevant function's byte range.
  typedef struct { uint32_t old, new; const char *desc; uintptr_t base; uintptr_t size; } BPatch;
  const BPatch bp[] = {
    { 0x1A980708, 0x2A1803E8, "MIN(mipmap) cinc->mov w8,w24 (NEAREST)",  iwm_base, iwm_size },
    { 0x1A9C0789, 0x2A1C03E9, "MIN cinc->mov w9,w28 (NEAREST)",          iwm_base, iwm_size },
    { 0x1A9C0782, 0x2A1C03E2, "MAG cinc->mov w2,w28 (NEAREST)",          iwm_base, iwm_size },
    { 0x5284C02A, 0x5284C00A, "VolatileMgr GL_LINEAR->GL_NEAREST",       iwm_base, iwm_size },
    { 0x5284E029, 0x5284E009, "VolatileMgr GL_LINEAR_MIPMAP->GL_NEAREST_MIPMAP", iwm_base, iwm_size },
    { 0x5284E035, 0x5284E015, "setAntiAlias MIN GL_LINEAR_MIPMAP->GL_NEAREST_MIPMAP", aap_base, aap_size },
    { 0x5284C036, 0x5284C016, "setAntiAlias MIN GL_LINEAR->GL_NEAREST",  aap_base, aap_size },
    { 0x5284C022, 0x5284C002, "setAntiAlias MAG GL_LINEAR->GL_NEAREST",  aap_base, aap_size },
  };

  const int n = (int)(sizeof(bp) / sizeof(bp[0]));
  for (int i = 0; i < n; i++) {
    uint32_t old_le = bp[i].old;
    uint32_t new_le = bp[i].new;
    // Scan the function body for the old word.
    int found = 0;
    for (uintptr_t a = bp[i].base; a + 4 <= bp[i].base + bp[i].size; a += 4) {
      uint32_t cur;
      __builtin_memcpy(&cur, (void *)a, 4);
      if (cur == new_le) { found = 1; break; } // already patched
      if (cur == old_le) {
        __builtin_memcpy((void *)a, &new_le, 4);
        debugPrintf("patches: bilinear %08x->%08x @ 0x%lx  %s\n",
                    old_le, new_le, (unsigned long)(a - (uintptr_t)mod->load_base),
                    bp[i].desc);
        found = 1;
        break;
      }
    }
    if (!found)
      debugPrintf("patches: bilinear -- pattern %08x not found in function (%s)\n",
                  old_le, bp[i].desc);
  }
}

// ---------------------------------------------------------------------------
// Public entry point: apply all enabled patches.
// Call AFTER ct_resolve_imports() and resolve_entry_points(), but BEFORE
// so_finalize() -- load_base is writable at this point.
// ---------------------------------------------------------------------------
static inline void apply_game_patches(so_module *mod) {
  // These are included from config.h via the caller.
  extern Config config;

  if (config.cursor_fix) {
    debugPrintf("patches: applying cursor_fix\n");
    apply_patches(mod, g_cursor_patches, PATCH_COUNT(g_cursor_patches));
  }

  if (config.remove_bilinear_filter) {
    debugPrintf("patches: applying remove_bilinear\n");
    apply_bilinear_patches(mod);
  }

  if (config.remove_mobile_ui) {
    debugPrintf("patches: applying remove_mobile_ui\n");
    apply_patches(mod, g_mobile_ui_patches, PATCH_COUNT(g_mobile_ui_patches));
  }

  if (config.controller_glyphs) {
    debugPrintf("patches: applying controller_glyphs\n");
    apply_patches(mod, g_glyph_patches, PATCH_COUNT(g_glyph_patches));
  }

  if (config.fix_diagonal_movement) {
    debugPrintf("patches: applying diagonal_movement fix\n");
    apply_patches(mod, g_diagonal_patches, PATCH_COUNT(g_diagonal_patches));
  }

  if (config.fixed_timestep) {
    debugPrintf("patches: applying fixed_timestep\n");
    apply_patches(mod, g_fixed_timestep_patches, PATCH_COUNT(g_fixed_timestep_patches));
  }

  if (config.field_zoom_fix) {
    debugPrintf("patches: applying field_zoom_fix (zoom=%g)\n", config.field_zoom);
    apply_patches(mod, g_field_zoom_fix_patches, PATCH_COUNT(g_field_zoom_fix_patches));
    // View/FOV size FIRST, blit scale second -- order doesn't matter functionally
    // (different instruction sites) but this reads in the order data flows:
    // decide how much map is captured, then how big that capture is drawn.
    apply_field_view_zoom(mod, config.field_zoom);
    apply_field_zoom(mod, config.field_zoom);
    // apply_field_scroll_anchor and apply_field_scroll_pivot_y are NOT
    // called -- both are kept above only as dead history (see their own
    // header comments for why each was superseded).
    //
    // CURRENT STATE, X and Y fixed via two completely different mechanisms
    // because they needed different ones (see apply_field_node_anchor's
    // header for the full history of why Y took five attempts):
    //
    //   X: apply_field_node_anchor shifts the fieldmap NODE itself --
    //      node_x = 320 - 128*zoom. Proven exact, ample real margin at
    //      every zoom, unchanged since it first worked.
    //
    //   Y: handled entirely inside apply_field_view_zoom's viewH formula
    //      now (see its header: Scroll's tY pins the visible window
    //      BOTTOM at camTop + viewH, so viewH positions the window --
    //      viewH = 112 + 173.333/zoom keeps the character at stock's
    //      186.67 design px at every zoom). Engine untouched, node
    //      untouched: node_y in apply_field_node_anchor stays permanently
    //      0.0 (node shifts for Y reintroduce black bars --
    //      hardware-confirmed, three times), and the round-7 engine-side
    //      attempt (apply_field_engine_registration_y below) is reverted
    //      -- it broke the battle camera (see its post-mortem).
    apply_field_node_anchor(mod, config.field_zoom);
    // apply_field_engine_center_y(mod, config.field_zoom_y_anchor_chips);
    // ^ DISABLED as of this session. Its premise -- that ResetScrollAddress's
    // 14/24 chip split directly sets the character's on-screen vertical
    // registration -- was verified only on the WRITE side (confirmed exactly
    // which values get stored to state+0x18/+0x1c) but never traced on the
    // READ side (what actually consumes those two fields downstream). Real
    // hardware test at chips=12, zoom=3.0 made things WORSE, not better: the
    // character sat lower and the bottom of the screen went missing / the
    // character rendered partially off-screen -- a different, worse failure
    // than before, which only makes sense if this window is NOT the
    // character's screen-position registration point.
    //
    // Full disassembly of the function (every branch, all 3 cases per axis,
    // the shared tail) shows the [Xmin,Xmax]/[Ymin,Ymax] pairs it produces
    // look like edge-aware camera/scroll BOUNDS (a dead-zone or map-edge
    // clamp range), not a direct registration offset -- consistent with the
    // regression, since narrowing/shifting that range could change WHEN
    // clamping kicks in without ever being what centers the character during
    // ordinary walking. Left fully documented and disabled (not deleted)
    // pending real data, rather than another blind patch.
    //
    // Current state: X is centered exactly right (apply_field_node_anchor,
    // proven correct at every zoom). Y is handled by apply_field_view_zoom's
    // character-pinned viewH formula (visible-window positioning; see its
    // header for the corrected model) -- node_y stays fixed 0.0, its
    // confirmed-safe, zero-black-bar, zero-clipping baseline, and the
    // engine camera is byte-for-byte stock again (battles depend on it).

    // TEMPORARY -- see apply_field_scroll_debug_log's header comment.
    // Logs Scroll()'s per-frame camera-follow state to debug.log so the
    // "drifts left/down while walking" bug can be diagnosed from real
    // numbers instead of screenshots. Remove this call once done.
    //
    // RE-ENABLED: the prior crash was isolated by pulling this exact call
    // -- with it removed, debug.log showed the game boot fully, write a
    // save (common.bin + slot + meta.bin) twice, with no crash. That
    // confirms the crash is gone with the hook off. Before re-enabling,
    // the cave (0xd0118) and entry patch were independently re-verified
    // against this build's actual libchrono.so: the target bytes are
    // still genuinely zero, the ENTRY instruction is still the expected
    // stock word, both PT_LOAD-segment and instruction/branch-encoding
    // checks pass, and this loader (so_util.c) never reads the ELF
    // section that cave address happens to sit in (checked -- no "hash"
    // reference anywhere in the loader), so it's safe to write over.
    apply_field_scroll_debug_log(mod);
  }

  if (config.map_zoom_fix) {
    debugPrintf("patches: applying map_zoom_fix (zoom=%g)\n", config.map_zoom);
    apply_map_zoom(mod, config.map_zoom);
    // Closes the centering gap map_zoom's own header (and config.h's
    // map_zoom_fix comment) documents as still-open: apply_map_zoom
    // alone only resizes the WorldMap node, around its old zoom-blind
    // anchor -- see apply_map_node_anchor's header for the full disassembly
    // and for why its Y half specifically needs real-hardware confirmation
    // before trusting it away from the default zoom.
    apply_map_node_anchor(mod, config.map_zoom);
  }

  if (config.game_area_width_fix) {
    debugPrintf("patches: applying game_area_width_fix\n");
    apply_patches(mod, g_gamearea_patches, PATCH_COUNT(g_gamearea_patches));
  }

  if (config.menu_alignment_fix) {
    debugPrintf("patches: applying menu_alignment_fix\n");
    apply_patches(mod, g_text_alignment_patches, PATCH_COUNT(g_text_alignment_patches));
  }

  if (config.ui_scale_fix) {
    // 640x360 stamped across the WHOLE aspect table, both modes: 2x at 720p
    // handheld, 3x at 1080p docked -- the familiar framing. Table-wide
    // stamping keeps it stable across boot/dock/undock (the game's runtime
    // aspect picker can no longer swap entries).
    debugPrintf("patches: design resolution 640x360 (all aspect-table entries)\n");
    apply_ui_scale_fix(mod, 640.0f, 360.0f);
  }
}

#endif /* __PATCHES_H__ */
