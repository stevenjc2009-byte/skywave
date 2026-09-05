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
