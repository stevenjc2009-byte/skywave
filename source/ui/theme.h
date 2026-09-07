#pragma once

// One place for every colour and dimension the UI draws with.
//
// The palette is a night sky with a warm signal-lamp accent: deep blue behind
// everything, a lighter blue for panels, amber for whatever is live. It is not
// decoration - on the 3DS's screens a saturated accent is the only thing that
// reads reliably at arm's length, and "which row is playing" is the one
// question the interface has to answer instantly.

#include <citro2d.h>

// C2D_Color32 takes r, g, b, a.
#define SW_BG          C2D_Color32(0x0C, 0x14, 0x24, 0xFF)  // deep night blue
#define SW_PANEL       C2D_Color32(0x15, 0x20, 0x38, 0xFF)
#define SW_PANEL_HI    C2D_Color32(0x1E, 0x2D, 0x4C, 0xFF)
#define SW_LINE        C2D_Color32(0x2A, 0x3C, 0x5E, 0xFF)

#define SW_TEXT        C2D_Color32(0xEC, 0xF1, 0xF8, 0xFF)
#define SW_TEXT_DIM    C2D_Color32(0x8A, 0x9B, 0xB4, 0xFF)
#define SW_TEXT_FAINT  C2D_Color32(0x5A, 0x6B, 0x84, 0xFF)

#define SW_ACCENT      C2D_Color32(0xFF, 0xB3, 0x3C, 0xFF)  // signal amber
#define SW_ACCENT_DIM  C2D_Color32(0x7A, 0x55, 0x1C, 0xFF)
#define SW_GOOD        C2D_Color32(0x5D, 0xD3, 0x9E, 0xFF)
#define SW_BAD         C2D_Color32(0xFF, 0x6B, 0x6B, 0xFF)

#define SW_TOP_W       400
#define SW_BOT_W       320
#define SW_SCREEN_H    240

// Bottom screen layout. The row height is the number everything else follows
// from: 34 px fits five rows between the tabs and the footer, which is as many
// as stay readable with a subtitle under each name.
#define SW_TAB_H       28
#define SW_ROW_H       34
#define SW_FOOTER_H    26

// The heading gets a strip of its own between the tabs and the first row. It
// had none to begin with and was drawn at the top of the list area, straight
// through the first station's name - readable in a screenshot only as a smear.
#define SW_HEAD_H      16
#define SW_HEAD_Y      (SW_TAB_H + 2)

#define SW_LIST_Y      (SW_TAB_H + SW_HEAD_H)
#define SW_LIST_H      (SW_SCREEN_H - SW_LIST_Y - SW_FOOTER_H)
#define SW_ROWS_VISIBLE (SW_LIST_H / SW_ROW_H)

// Added for the visual pass below - a low twin of the background and panel
// colours for gradients, a soft shadow for depth under the chrome, a dimmer
// accent for chips/fills, and twelve on-palette hues for the procedural
// station-monogram tiles. Additive only: nothing above is renamed or removed.
#define SW_BG_LOW      C2D_Color32(0x07, 0x0C, 0x18, 0xFF)  // ~7 RGB steps below SW_BG
#define SW_PANEL_LOW   C2D_Color32(0x10, 0x18, 0x2C, 0xFF)  // ~5 RGB steps below SW_PANEL
#define SW_SHADOW      C2D_Color32(0x00, 0x00, 0x00, 0x50)
#define SW_SHADOW_0    C2D_Color32(0x00, 0x00, 0x00, 0x00)  // transparent end of a shadow gradient
#define SW_ACCENT_SOFT C2D_Color32(0xFF, 0xB3, 0x3C, 0x40)

// Same packing C2D_Color32 does, written as a macro because that function is
// only constexpr when citro2d is compiled as C++. In C it is a static inline,
// which cannot initialise a file-scope array - the build failed with
// "initializer element is not constant" on all twelve entries below. The
// layout is copied straight from c2d/base.h:104.
#define SW_RGBA(r, g, b, a) \
    ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))

static const u32 SW_TILE[12] = {
    SW_RGBA(0xC9, 0x5A, 0x5A, 0xFF),  // red
    SW_RGBA(0xC9, 0x82, 0x46, 0xFF),  // orange
    SW_RGBA(0xC2, 0xA6, 0x3C, 0xFF),  // gold
    SW_RGBA(0x96, 0xB4, 0x46, 0xFF),  // lime
    SW_RGBA(0x4E, 0xA8, 0x5C, 0xFF),  // green
    SW_RGBA(0x3E, 0xAE, 0x9A, 0xFF),  // teal
    SW_RGBA(0x3E, 0x9E, 0xC2, 0xFF),  // sky
    SW_RGBA(0x5A, 0x82, 0xD9, 0xFF),  // blue
    SW_RGBA(0x82, 0x66, 0xD9, 0xFF),  // violet
    SW_RGBA(0xB0, 0x5A, 0xC2, 0xFF),  // magenta
    SW_RGBA(0xC9, 0x5A, 0x8C, 0xFF),  // pink
    SW_RGBA(0x82, 0x86, 0x96, 0xFF),  // slate (neutral filler)
};
