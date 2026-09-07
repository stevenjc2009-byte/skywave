#include "ui.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

#include "theme.h"
#include "../version.h"

static C3D_RenderTarget *s_top;
static C3D_RenderTarget *s_bot;
static C2D_TextBuf       s_buf;

// Touch tracking. A row is chosen on release rather than on press so that a
// touch that turns into a drag scrolls the list instead of playing whatever
// happened to be under the stylus when it landed.
static bool  s_touching;
static int   s_touch_x0, s_touch_y0;
static int   s_touch_x,  s_touch_y;
static bool  s_dragged;

// Which row (if any) is still lit from a recent touch release, and how many
// frames of fade it has left. Grouped with the touch statics above because it
// is driven by the same touch state - see the release path in swUiFrame.
static int s_flash_row = -1;
static int s_flash_ttl = 0;

// Detects the modal opening or closing so a touch that straddles the
// transition cannot be replayed into the wrong input path. Opening or closing
// the modal mid-touch would otherwise leave s_touching true and hand the next
// release to whichever branch is now active - see swUiFrame.
static SwUiModal s_prev_modal = SW_MODAL_NONE;

// Animation clock for the level bars and the tab-indicator glide, and nothing
// else. Incremented once per swUiFrame call so app.c never has to know it
// exists.
static u32 s_frame;

// Filter-modal geometry. Shared between draw_modal() and the modal's own
// touch handling further down in this file - the two must agree pixel for
// pixel or a tap lands on the wrong row. Budget: 24 title + 24 segmented +
// 5*28 list + 28 footer = 216, inside the 220 px panel.
#define SW_MODAL_X             10.0f
#define SW_MODAL_Y             10.0f
#define SW_MODAL_W             (SW_BOT_W - 20.0f)
#define SW_MODAL_H             220.0f
#define SW_MODAL_TITLE_H       24.0f
#define SW_MODAL_SEG_H         24.0f
#define SW_MODAL_ROW_H         28.0f
#define SW_MODAL_ROWS_VISIBLE  5
#define SW_MODAL_FOOTER_H      28.0f

static const char *kModalPageNames[2] = { "Country", "Bitrate" };

// A small static table rather than a real FFT or even a sine() call - this
// only ever needs to look roughly like motion, and it is scaled by the real
// buffer percentage wherever it is used, so it goes flat the instant the
// stream actually stalls instead of pretending to keep listening.
static const float kLevelSine[16] = {
    0.50f, 0.71f, 0.87f, 0.98f, 0.98f, 0.87f, 0.71f, 0.50f,
    0.29f, 0.13f, 0.02f, 0.02f, 0.13f, 0.29f, 0.50f, 0.50f
};

static int row_at(const SwUi *st, int x, int y);  // defined below; draw_rows needs it

// FNV-1a, for turning a station name into one of the twelve monogram hues in
// theme.h. Deterministic and stateless - the same name always lands on the
// same colour without storing anything per station.
static u32 fnv1a(const char *s)
{
    u32 h = 2166136261u;
    while (*s) { h ^= (u8)(unsigned char)*s++; h *= 16777619u; }
    return h;
}

// citro2d has no rounded-rect primitive. Two rects plus four circles gets one
// for six draws - the same cost as any other flat colour on this hardware,
// since C2D_DrawRectangle/DrawCircleSolid do not get cheaper for being plain.
static void rounded_rect(float x, float y, float w, float h, float r, u32 clr)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0)     r = 0;

    C2D_DrawRectSolid(x + r, y,         0.0f, w - 2 * r, h,         clr);
    C2D_DrawRectSolid(x,     y + r,     0.0f, w,         h - 2 * r, clr);
    C2D_DrawCircleSolid(x + r,     y + r,     0.0f, r, clr);
    C2D_DrawCircleSolid(x + w - r, y + r,     0.0f, r, clr);
    C2D_DrawCircleSolid(x + r,     y + h - r, 0.0f, r, clr);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.0f, r, clr);
}

static const char *kTabNames[SW_TAB_COUNT] = { "Browse", "Saved", "Search", "About" };

// ------------------------------------------------------------------ text

// Draws `str` at (x, y), shortened with an ellipsis if it will not fit in
// `maxw`. Truncating rather than clipping matters: a station name cut off
// mid-letter looks like a rendering bug, one ending in "..." reads as a name
// that is simply longer than the screen.
static void text(float x, float y, float scale, u32 colour, float maxw, const char *str)
{
    if (!str || !*str) return;

    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s", str);

    C2D_Text t;
    float w = 0, h = 0;

    C2D_TextParse(&t, s_buf, tmp);
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);

    if (maxw > 0 && w > maxw) {
        // Binary search rather than trimming a character at a time. Every
        // measurement costs a C2D_TextParse, and a parse allocates out of the
        // shared text buffer for the rest of the frame - a linear trim of a
        // long station name would exhaust it and later text would silently stop
        // drawing. This is bounded at about eight parses.
        char full[192];
        snprintf(full, sizeof(full), "%s", str);

        size_t lo = 0, hi = strlen(full);
        size_t best = 0;

        while (lo <= hi) {
            size_t mid = (lo + hi) / 2;
            if (mid + 3 >= sizeof(tmp)) break;

            memcpy(tmp, full, mid);
            tmp[mid] = '.'; tmp[mid + 1] = '.'; tmp[mid + 2] = '.'; tmp[mid + 3] = 0;

            C2D_TextParse(&t, s_buf, tmp);
            C2D_TextGetDimensions(&t, scale, scale, &w, &h);

            if (w <= maxw) { best = mid; lo = mid + 1; }
            else { if (mid == 0) break; hi = mid - 1; }
        }

        memcpy(tmp, full, best);
        tmp[best] = '.'; tmp[best + 1] = '.'; tmp[best + 2] = '.'; tmp[best + 3] = 0;
        C2D_TextParse(&t, s_buf, tmp);
    }

    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, colour);
}

// Same, centred in a box starting at `x` of width `w`.
static void text_centre(float x, float y, float w, float scale, u32 colour, const char *str)
{
    if (!str || !*str) return;

    C2D_Text t;
    float tw = 0, th = 0;
    C2D_TextParse(&t, s_buf, str);
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);

    if (tw > w) { text(x, y, scale, colour, w, str); return; }
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x + (w - tw) / 2, y, 0.5f, scale, scale, colour);
}

// ------------------------------------------------------------------ lifetime

bool swUiInit(void)
{
    gfxInitDefault();

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) { C3D_Fini(); return false; }
    C2D_Prepare();

    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    // 8192, not 4096: this counts glyphs, not bytes, and the truncation binary
    // search in text() does up to ~8 C2D_TextParse calls per truncated string,
    // each allocating the trial string's glyphs out of this buffer. Five rows
    // of two lines already approached the old limit before the search bar,
    // filter modal and now-playing card below added more text per frame -
    // past the limit, text just stops drawing, with no error.
    //
    // Raised to 16384 after an audit added up the worst case rather than
    // guessing at it: five list rows of a truncated 60-odd-character name plus
    // a subtitle, each costing up to eight parses of the binary search, plus
    // the top screen's now-playing text, came to roughly 9,500 glyphs against
    // a ceiling of 8,192. Nothing visibly broke, because the failure mode is
    // silent - text simply stops appearing, with no error and no crash - which
    // is exactly why the margin is worth paying for. The cost is one allocation
    // at startup, not per frame.
    s_buf = C2D_TextBufNew(16384);

    return s_top && s_bot && s_buf;
}

void swUiExit(void)
{
    if (s_buf) { C2D_TextBufDelete(s_buf); s_buf = NULL; }
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

bool swUiRunning(void) { return aptMainLoop(); }

// -------------------------------------------------------------- top screen

static void draw_top(const SwUi *st)
{
    C2D_TargetClear(s_top, SW_BG);
    C2D_SceneBegin(s_top);

    // Gradient ground: one rectangle spanning the whole screen, its top
    // corners SW_BG and its bottom corners SW_BG_LOW. A gradient costs exactly
    // the same as a flat rect on this hardware - the whole reason "bland" was
    // free to fix.
    C2D_DrawRectangle(0, 0, 0.0f, SW_TOP_W, SW_SCREEN_H, SW_BG, SW_BG, SW_BG_LOW, SW_BG_LOW);

    // Header strip, with a soft shadow fading into the gradient below it so it
    // reads as sitting above the page instead of being a flat band painted on.
    C2D_DrawRectSolid(0, 0, 0.0f, SW_TOP_W, 26, SW_PANEL);
    C2D_DrawRectangle(0, 26, 0.0f, SW_TOP_W, 3, SW_SHADOW, SW_SHADOW, SW_SHADOW_0, SW_SHADOW_0);
    text(12, 5, 0.62f, SW_ACCENT, 200, "SKYWAVE");

    char ver[32];
    snprintf(ver, sizeof(ver), "v%s", SKYWAVE_VERSION);
    text(SW_TOP_W - 60, 8, 0.44f, SW_TEXT_FAINT, 54, ver);

    bool loaded = st->station && st->station[0];

    if (!loaded) {
        text_centre(0, 100, SW_TOP_W, 0.66f, SW_TEXT_DIM, "Nothing playing");
        text_centre(0, 128, SW_TOP_W, 0.46f, SW_TEXT_FAINT,
                    "Pick a station on the touch screen.");
    } else {
        // Now-playing card: a rounded panel holding a procedural monogram
        // tile plus the station's text, so the top screen has one clear focal
        // point instead of three lines floating on a flat ground. The tile's
        // colour and letters come entirely from the station's own name - no
        // artwork, no third-party branding, which this project forbids.
        float cx = 16, cy = 36, cw = SW_TOP_W - 32, ch = 92;
        rounded_rect(cx, cy, cw, ch, 10, SW_PANEL);

        float tsz = 56, tx = cx + 12, ty = cy + 12;
        u32 tile_clr = SW_TILE[fnv1a(st->station) % 12];
        rounded_rect(tx, ty, tsz, tsz, 10, tile_clr);

        char mono[3] = { 0, 0, 0 };
        {
            int m = 0;
            for (const char *p = st->station; *p && m < 2; p++) {
                char c = *p;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                    mono[m++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
                }
            }
        }
        text_centre(tx, ty + tsz / 2 - 12, tsz, 0.62f, SW_TEXT, mono);

        // A level animation driven by buffer_pct, not a real spectrum - see
        // kLevelSine above. Only while actually playing: a permanently
        // wiggling idle screen would be worse than a still one.
        if (st->is_live) {
            float scale = st->buffer_pct > 0 ? (float)st->buffer_pct / 100.0f : 0.0f;
            float lx = tx, ly = ty + tsz - 2, bw = 3, gap = 2;
            for (int b = 0; b < 8; b++) {
                float ph = kLevelSine[(s_frame / 3 + (u32)b * 2) % 16];
                float bh = 2 + ph * 10 * scale;
                C2D_DrawRectSolid(lx + b * (bw + gap), ly - bh, 0.0f, bw, bh, SW_ACCENT);
            }
        }

        float ix = tx + tsz + 12, iw = cx + cw - ix - 12;

        text(ix, cy + 10, 0.60f, SW_TEXT, iw, st->station);

        if (st->now && st->now[0])
            text(ix, cy + 42, 0.46f, SW_ACCENT, iw, st->now);
        else
            text(ix, cy + 42, 0.46f, SW_TEXT_FAINT, iw,
                 "This station sends no track information.");

        text(ix, cy + 68, 0.44f, SW_TEXT_DIM, iw, st->status);
    }

    // Buffer bar. Showing it is what makes a dropout read as a weak connection
    // rather than as the app being broken.
    if (st->buffer_pct >= 0) {
        float bx = 20, by = 150, bw = SW_TOP_W - 40, bh = 8;
        int pct = st->buffer_pct; if (pct > 100) pct = 100; if (pct < 0) pct = 0;

        C2D_DrawRectSolid(bx, by, 0.0f, bw, bh, SW_PANEL);
        C2D_DrawRectSolid(bx, by, 0.0f, bw * (float)pct / 100.0f, bh,
                          pct < 15 ? SW_BAD : SW_GOOD);
        text(bx, by + 12, 0.42f, SW_TEXT_FAINT, bw, "Buffer");
    }

    // Volume.
    {
        float vx = 20, vy = 186, vw = SW_TOP_W - 40, vh = 6;
        C2D_DrawRectSolid(vx, vy, 0.0f, vw, vh, SW_PANEL);
        C2D_DrawRectSolid(vx, vy, 0.0f, vw * (float)st->volume / 100.0f, vh, SW_ACCENT_DIM);

        char v[48];
        snprintf(v, sizeof(v), "Volume %d%%   (L / R to change)", st->volume);
        text(vx, vy + 10, 0.42f, SW_TEXT_FAINT, vw, v);
    }

    if (st->message && st->message[0])
        text(20, 218, 0.46f, st->message_bad ? SW_BAD : SW_GOOD,
             SW_TOP_W - 40, st->message);
}

// ----------------------------------------------------------- bottom screen

static void draw_tabs(const SwUi *st)
{
    // Active-tab highlight glides toward the selected tab at a fixed fraction
    // of the remaining distance per frame, rather than snapping - so changing
    // tabs reads as one continuous motion. Plain C2D_DrawRectSolid state, no
    // scissor test, so it costs nothing extra and cannot black out real
    // hardware the way a scissored list-slide transition could (see item 4's
    // notes on why that was rejected).
    static float s_tab_glide = -1.0f;

    float w = (float)SW_BOT_W / SW_TAB_COUNT;
    float target = w * (float)st->tab;

    if (s_tab_glide < 0) s_tab_glide = target;  // no glide on the very first frame
    s_tab_glide += (target - s_tab_glide) * 0.35f;

    C2D_DrawRectSolid(0, 0, 0.0f, SW_BOT_W, SW_TAB_H, SW_PANEL);
    C2D_DrawRectSolid(s_tab_glide, 0, 0.0f, w, SW_TAB_H, SW_PANEL_HI);
    C2D_DrawRectSolid(s_tab_glide, SW_TAB_H - 3, 0.0f, w, 3, SW_ACCENT);

    for (int i = 0; i < SW_TAB_COUNT; i++) {
        bool on = (int)st->tab == i;
        text_centre(w * i, 6, w, 0.48f, on ? SW_TEXT : SW_TEXT_DIM, kTabNames[i]);

        // Update-available badge (item 5): a durable passive signal that
        // survives whatever `message` is showing this frame.
        if (st->tab_badge[i])
            C2D_DrawCircleSolid(w * (i + 1) - 8, 5, 0.0f, 3, SW_ACCENT);
    }

    C2D_DrawRectSolid(0, SW_TAB_H, 0.0f, SW_BOT_W, 1, SW_LINE);
}

static void draw_rows(const SwUi *st)
{
    if (st->row_count == 0) {
        text_centre(10, SW_LIST_Y + 60, SW_BOT_W - 20, 0.48f, SW_TEXT_FAINT,
                    st->empty_text ? st->empty_text : "Nothing here yet.");
        return;
    }

    for (int v = 0; v <= SW_ROWS_VISIBLE; v++) {
        int i = st->scroll + v;
        if (i < 0 || i >= st->row_count) continue;

        float y = SW_LIST_Y + v * SW_ROW_H;
        if (y + SW_ROW_H > SW_LIST_Y + SW_LIST_H) continue;

        const SwUiRow *r = &st->rows[i];
        bool sel     = (i == st->selected);
        bool pressed = st->modal == SW_MODAL_NONE && s_touching && !s_dragged &&
                       row_at(st, s_touch_x, s_touch_y) == i;
        bool flashed = !pressed && i == s_flash_row && s_flash_ttl > 0;

        // Subtle zebra so a dense two-line list stays scannable without
        // relying on the separator line alone.
        if (v % 2 == 1)
            C2D_DrawRectSolid(0, y, 0.0f, SW_BOT_W, SW_ROW_H - 2, SW_PANEL_LOW);

        // Touch feedback: the row currently under the stylus lights up while
        // held, and a released row keeps a short decaying flash. Neither was
        // ever drawn before, which is why every tap felt dead until the
        // network answered.
        if (pressed || sel)
            rounded_rect(2, y + 1, SW_BOT_W - 4, SW_ROW_H - 4, 6, SW_PANEL_HI);
        if (flashed) {
            u8 a = (u8)(0x60 * s_flash_ttl / 8);
            rounded_rect(2, y + 1, SW_BOT_W - 4, SW_ROW_H - 4, 6,
                        C2D_Color32(0xFF, 0xB3, 0x3C, a));
        }

        // The on-air stripe is a rounded pill rather than a flat bar - shape
        // as well as colour, so which station is playing survives even if the
        // accent colour alone would not (see the no-colour-alone constraint).
        if (r->playing)
            rounded_rect(3, y + 4, 4, SW_ROW_H - 10, 2, SW_ACCENT);

        float text_w = SW_BOT_W - 24 - (r->starred ? 18 : 0);
        text(12, y + 3,  0.50f, r->playing ? SW_ACCENT : SW_TEXT,     text_w, r->title);
        text(12, y + 19, 0.40f, SW_TEXT_DIM, text_w, r->sub);

        // An amber dot for a saved station, drawn rather than typed: the
        // console's system font has no star, and a missing glyph renders as a
        // blank box that reads as a bug.
        if (r->starred)
            C2D_DrawCircleSolid(SW_BOT_W - 14, y + SW_ROW_H / 2 - 1, 0.0f, 5, SW_ACCENT);

        // A faint separator between rows - shape cue for "next station",
        // skipped after the last visible row so it does not draw against the
        // scrollbar/footer edge.
        if (v < SW_ROWS_VISIBLE - 1)
            C2D_DrawRectSolid(8, y + SW_ROW_H - 2, 0.0f, SW_BOT_W - 16, 1, SW_LINE);
    }

    // Scrollbar, drawn only when there is something to scroll.
    if (st->row_count > SW_ROWS_VISIBLE) {
        float track_h = SW_LIST_H;
        float thumb_h = track_h * (float)SW_ROWS_VISIBLE / (float)st->row_count;
        float max_scroll = (float)(st->row_count - SW_ROWS_VISIBLE);
        float pos = max_scroll > 0 ? (float)st->scroll / max_scroll : 0;

        C2D_DrawRectSolid(SW_BOT_W - 3, SW_LIST_Y, 0.0f, 3, track_h, SW_PANEL);
        C2D_DrawRectSolid(SW_BOT_W - 3, SW_LIST_Y + (track_h - thumb_h) * pos,
                          0.0f, 3, thumb_h, SW_LINE);
    }
}

// A small accent-outlined badge (a filled circle in the accent colour with a
// slightly smaller panel-coloured circle on top, leaving a ring) holding a
// button's letter, drawn at (cx, cy). citro2d has no stroked-circle
// primitive, so this is the two-draw way to fake one.
static void glyph_badge(float cx, float cy, float r, const char *key, u32 bg)
{
    C2D_DrawCircleSolid(cx, cy, 0.0f, r,       SW_ACCENT);
    C2D_DrawCircleSolid(cx, cy, 0.0f, r - 1.5f, bg);
    text_centre(cx - r, cy - 6, r * 2, 0.38f, SW_ACCENT, key);
}

static void draw_footer(const SwUi *st)
{
    float y = SW_SCREEN_H - SW_FOOTER_H;
    C2D_DrawRectSolid(0, y, 0.0f, SW_BOT_W, SW_FOOTER_H, SW_PANEL);
    C2D_DrawRectSolid(0, y, 0.0f, SW_BOT_W, 1, SW_LINE);

    // Button glyphs instead of a run-on hint line: a fixed line goes stale the
    // instant its meaning changes underfoot - see item 1's bug, where "A:
    // search" kept printing long after A started playing a station instead.
    // A badge only shows an action that is true right now.
    struct { const char *key; const char *label; } hints[4];
    int n = 0;

    switch (st->tab) {
        case SW_TAB_SEARCH:
            if (st->row_count == 0) {
                hints[n].key = "A"; hints[n].label = "search"; n++;
            } else {
                hints[n].key = "A";   hints[n].label = "play";   n++;
                hints[n].key = "SEL"; hints[n].label = "search"; n++;
            }
            hints[n].key = "X"; hints[n].label = "stop";   n++;
            hints[n].key = "B"; hints[n].label = "filter"; n++;
            break;
        case SW_TAB_ABOUT:
            hints[n].key = "A"; hints[n].label = "choose"; n++;
            break;
        default:
            hints[n].key = "A"; hints[n].label = "play"; n++;
            hints[n].key = "Y"; hints[n].label = "save"; n++;
            hints[n].key = "X"; hints[n].label = "stop"; n++;
            break;
    }

    float slot = (SW_BOT_W - 12) / (float)n;
    float cy = y + SW_FOOTER_H / 2.0f;
    float r = 8;

    for (int i = 0; i < n; i++) {
        float slot_x = 6 + slot * i;
        bool  wide   = hints[i].key[1] != 0;  // "SEL" needs more than a circle's width
        float badge_w;

        if (wide) {
            badge_w = r * 2.6f;
            rounded_rect(slot_x, cy - r, badge_w, r * 2, r, SW_ACCENT);
            rounded_rect(slot_x + 1.5f, cy - r + 1.5f, badge_w - 3, r * 2 - 3, r - 1.5f, SW_PANEL);
            text_centre(slot_x, cy - 6, badge_w, 0.34f, SW_ACCENT, hints[i].key);
        } else {
            badge_w = r * 2;
            glyph_badge(slot_x + r, cy, r, hints[i].key, SW_PANEL);
        }

        float label_x = slot_x + badge_w + 4;
        float label_w = slot - badge_w - 8;
        if (label_w > 0) text(label_x, cy - 6, 0.38f, SW_TEXT_DIM, label_w, hints[i].label);
    }
}

// The filter modal. Copies the busy sheet's pattern (a translucent rect over
// the content, input captured by branching in swUiFrame) but this is the
// first real overlay in the app: it also covers the footer, since its own
// Apply/Clear footer stands in for the real one, and it needs its own touch
// geometry, kept in the SW_MODAL_* macros above so swUiFrame's input handling
// can agree with it pixel for pixel.
static void draw_modal(const SwUi *st)
{
    C2D_DrawRectSolid(0, 0, 0.0f, SW_BOT_W, SW_SCREEN_H, C2D_Color32(0x00, 0x00, 0x00, 0xA0));

    float px = SW_MODAL_X, py = SW_MODAL_Y, pw = SW_MODAL_W;
    rounded_rect(px, py, pw, SW_MODAL_H, 10, SW_PANEL);

    text_centre(px, py + 6, pw, 0.46f, SW_TEXT, "Filter stations");

    // Segmented control: Country | Bitrate.
    float seg_y = py + SW_MODAL_TITLE_H;
    float seg_w = pw / 2;
    for (int p = 0; p < 2; p++) {
        bool on = st->modal_page == p;
        if (on) C2D_DrawRectSolid(px + seg_w * p, seg_y, 0.0f, seg_w, SW_MODAL_SEG_H, SW_PANEL_HI);
        text_centre(px + seg_w * p, seg_y + 5, seg_w, 0.42f, on ? SW_ACCENT : SW_TEXT_DIM,
                    kModalPageNames[p]);
    }
    C2D_DrawRectSolid(px + seg_w, seg_y + 4, 0.0f, 1, SW_MODAL_SEG_H - 8, SW_LINE);

    // Scrolling list - reuses st->rows[]/row_count exactly as the main list
    // does, just at the modal's own row height, selection and scroll.
    float list_y = seg_y + SW_MODAL_SEG_H;

    if (st->row_count == 0)
        text_centre(px, list_y + 50, pw, 0.42f, SW_TEXT_FAINT, "Nothing to pick.");

    for (int v = 0; v < SW_MODAL_ROWS_VISIBLE; v++) {
        int i = st->modal_scroll + v;
        if (i < 0 || i >= st->row_count) continue;

        float y = list_y + v * SW_MODAL_ROW_H;
        const SwUiRow *r = &st->rows[i];
        bool sel = (i == st->modal_sel);

        if (sel) C2D_DrawRectSolid(px + 4, y + 1, 0.0f, pw - 8, SW_MODAL_ROW_H - 2, SW_PANEL_HI);

        // `playing` is reused here to mean "this is the currently applied
        // value", not "on air" - the same "here is the active one" shape cue,
        // repurposed for a different list.
        if (r->playing)
            C2D_DrawCircleSolid(px + pw - 16, y + SW_MODAL_ROW_H / 2, 0.0f, 4, SW_ACCENT);

        text(px + 12, y + 6, 0.44f, sel ? SW_ACCENT : SW_TEXT, pw - 32, r->title);
    }

    if (st->row_count > SW_MODAL_ROWS_VISIBLE) {
        float track_h  = SW_MODAL_ROWS_VISIBLE * SW_MODAL_ROW_H;
        float thumb_h  = track_h * (float)SW_MODAL_ROWS_VISIBLE / (float)st->row_count;
        float max_scroll = (float)(st->row_count - SW_MODAL_ROWS_VISIBLE);
        float pos = max_scroll > 0 ? (float)st->modal_scroll / max_scroll : 0;
        C2D_DrawRectSolid(px + pw - 4, list_y, 0.0f, 3, track_h, SW_PANEL);
        C2D_DrawRectSolid(px + pw - 4, list_y + (track_h - thumb_h) * pos, 0.0f, 3, thumb_h, SW_LINE);
    }

    // Footer: Apply / Clear. B also gets out, without applying.
    float foot_y = list_y + SW_MODAL_ROWS_VISIBLE * SW_MODAL_ROW_H;
    C2D_DrawRectSolid(px, foot_y, 0.0f, pw, 1, SW_LINE);

    float bh = SW_MODAL_FOOTER_H - 8;
    rounded_rect(px + 8, foot_y + 4, seg_w - 16, bh, 6, SW_ACCENT_DIM);
    text_centre(px + 8, foot_y + 4 + bh / 2 - 6, seg_w - 16, 0.40f, SW_TEXT, "Apply");
    rounded_rect(px + seg_w + 8, foot_y + 4, seg_w - 16, bh, 6, SW_PANEL_HI);
    text_centre(px + seg_w + 8, foot_y + 4 + bh / 2 - 6, seg_w - 16, 0.40f, SW_TEXT_DIM, "Clear");
}

static void draw_bottom(const SwUi *st)
{
    C2D_TargetClear(s_bot, SW_BG);
    C2D_SceneBegin(s_bot);

    // Gradient ground, same technique as the top screen.
    C2D_DrawRectangle(0, 0, 0.0f, SW_BOT_W, SW_SCREEN_H, SW_BG, SW_BG, SW_BG_LOW, SW_BG_LOW);

    // The filter modal REPLACES the page rather than being layered over it.
    // Every rect in this file draws at z=0.0 and every string at z=0.5 (see
    // text()), and citro2d depth-tests, so a string always wins against a
    // rectangle no matter which order the two were issued in. The modal's
    // opaque SW_PANEL body therefore does not hide the list behind it - the
    // emulator run showed every filter entry twice, once at the main list's
    // 34 px pitch out at the left margin and again at the modal's own indented
    // 28 px pitch, worst on the Bitrate page where "129 - 192 kbps" collided
    // with itself into mush. Drawing nothing underneath is the fix that cannot
    // regress; giving the modal a higher depth would only move the same
    // collision onto whatever gets layered over it next.
    // ...but `busy` outranks it, because swUiFrame's input handling below
    // blocks ALL input while busy and only then considers the modal. Drawing
    // them in the other order put a fully live-looking filter modal on screen
    // whose every button was dead - the worst of both, since the user cannot
    // see that the app is working and cannot act on what it is showing them.
    // The two orderings must stay in agreement: draw what the input handler
    // will actually respond to.
    if (st->modal != SW_MODAL_NONE && !st->busy) {
        draw_modal(st);
        return;
    }

    draw_tabs(st);

    // Depth on the chrome: a soft shadow under the strip above the list and
    // again just above the footer, so both read as sitting above the page
    // rather than as flat bands painted straight onto it.
    C2D_DrawRectangle(0, SW_LIST_Y, 0.0f, SW_BOT_W, 3, SW_SHADOW, SW_SHADOW, SW_SHADOW_0, SW_SHADOW_0);
    C2D_DrawRectangle(0, SW_SCREEN_H - SW_FOOTER_H - 3, 0.0f, SW_BOT_W, 3,
                      SW_SHADOW_0, SW_SHADOW_0, SW_SHADOW, SW_SHADOW);

    if (st->show_search_bar) {
        // Search bar and filter chip share the head strip. The chip is
        // deliberately narrow - it only ever shows "filtered" or not, the
        // detail lives in the modal.
        float bar_w = SW_BOT_W - 46;
        C2D_DrawRectSolid(6, SW_HEAD_Y - 1, 0.0f, bar_w, SW_HEAD_H, SW_PANEL_LOW);
        bool has_text = st->list_title && st->list_title[0];
        text(10, SW_HEAD_Y, 0.42f, has_text ? SW_TEXT : SW_TEXT_FAINT, bar_w - 8,
             has_text ? st->list_title : "SELECT to search");

        float chip_x = SW_BOT_W - 40;
        C2D_DrawRectSolid(chip_x, SW_HEAD_Y - 1, 0.0f, 34, SW_HEAD_H, SW_ACCENT_DIM);
        text_centre(chip_x, SW_HEAD_Y, 34, 0.36f, SW_TEXT, "Filter");
    } else if (st->list_title && st->list_title[0]) {
        text(8, SW_HEAD_Y, 0.42f, SW_TEXT_FAINT, SW_BOT_W - 16, st->list_title);
    }

    if (st->busy) {
        // A flat dim sheet over the list, so it is obvious that touching it
        // will do nothing rather than the app merely feeling unresponsive.
        // The rows are skipped rather than drawn under it for the same reason
        // the modal returns early above: this sheet is a rectangle, so it
        // cannot cover row text, and every name would read straight through it
        // at full brightness as though nothing were happening.
        C2D_DrawRectSolid(0, SW_LIST_Y, 0.0f, SW_BOT_W, SW_LIST_H,
                          C2D_Color32(0x0C, 0x14, 0x24, 0xC0));
        text_centre(0, SW_LIST_Y + 70, SW_BOT_W, 0.55f, SW_ACCENT, "Working...");
    } else {
        draw_rows(st);
    }

    draw_footer(st);
}

// ------------------------------------------------------------------- input

static void clamp_scroll(SwUi *st)
{
    int max_scroll = st->row_count - SW_ROWS_VISIBLE;
    if (max_scroll < 0) max_scroll = 0;
    if (st->scroll > max_scroll) st->scroll = max_scroll;
    if (st->scroll < 0) st->scroll = 0;
}

// Keeps the selected row on screen without ever jumping the list further than
// it has to - moving one row should scroll by one row, not recentre.
static void follow_selection(SwUi *st)
{
    if (st->row_count == 0) { st->selected = 0; st->scroll = 0; return; }

    if (st->selected < 0) st->selected = 0;
    if (st->selected >= st->row_count) st->selected = st->row_count - 1;

    if (st->selected < st->scroll) st->scroll = st->selected;
    if (st->selected >= st->scroll + SW_ROWS_VISIBLE)
        st->scroll = st->selected - SW_ROWS_VISIBLE + 1;

    clamp_scroll(st);
}

// Same idea as follow_selection, but for the modal's own selection/scroll
// pair and its own five-visible-rows geometry.
static void modal_follow(SwUi *st)
{
    if (st->row_count == 0) { st->modal_sel = 0; st->modal_scroll = 0; return; }

    if (st->modal_sel < 0) st->modal_sel = 0;
    if (st->modal_sel >= st->row_count) st->modal_sel = st->row_count - 1;

    if (st->modal_sel < st->modal_scroll) st->modal_scroll = st->modal_sel;
    if (st->modal_sel >= st->modal_scroll + SW_MODAL_ROWS_VISIBLE)
        st->modal_scroll = st->modal_sel - SW_MODAL_ROWS_VISIBLE + 1;

    int max_scroll = st->row_count - SW_MODAL_ROWS_VISIBLE;
    if (max_scroll < 0) max_scroll = 0;
    if (st->modal_scroll > max_scroll) st->modal_scroll = max_scroll;
    if (st->modal_scroll < 0) st->modal_scroll = 0;
}

// Which row a touch at (x, y) is over, or -1.
static int row_at(const SwUi *st, int x, int y)
{
    (void)x;
    if (y < SW_LIST_Y || y >= SW_LIST_Y + SW_LIST_H) return -1;

    int v = (y - SW_LIST_Y) / SW_ROW_H;
    if (v >= SW_ROWS_VISIBLE) return -1;

    int i = st->scroll + v;
    return (i >= 0 && i < st->row_count) ? i : -1;
}

SwUiAction swUiFrame(SwUi *st)
{
    SwUiAction act = { SW_ACT_NONE, -1 };

    hidScanInput();
    u32 down = hidKeysDown();
    u32 held = hidKeysHeld();
    u32 up   = hidKeysUp();

    s_frame++;
    if (s_flash_ttl > 0) s_flash_ttl--;

    // Opening or closing the modal resets the touch-tracking statics below.
    // Without this, a touch that straddles the transition leaves s_touching
    // true and hands the next release to whichever input branch is now
    // active - e.g. dismissing the modal by touch would otherwise immediately
    // play whatever row sits under the stylus on the list beneath it. This is
    // checked before the busy/modal/else split below so it always fires,
    // however the transition happened.
    if (st->modal != s_prev_modal) {
        s_touching   = false;
        s_dragged    = false;
        s_prev_modal = st->modal;
    }

    // Discards exactly one frame of input - see SwUi.swallow_frame. Set after
    // returning from the system keyboard: the applet's own confirming press
    // can otherwise be read as this app's next A on the very next frame and
    // play row 0 unasked.
    if (st->swallow_frame) {
        st->swallow_frame = false;
        down = held = up = 0;
    }

    follow_selection(st);

    if (st->busy) {
        // Busy blocks every action that could change what the worker thread is
        // reading - but NOT quitting. START stayed live here because `busy` is
        // cleared by a background job finishing, so a job that never finishes
        // leaves the user with no working button at all and no way out but the
        // power switch. Quitting is the one request that is always safe to
        // honour: app.c's exit path joins the worker before tearing anything
        // down, so this cannot free state the job is still using.
        if (down & KEY_START) act.kind = SW_ACT_QUIT;
    } else if (st->modal != SW_MODAL_NONE) {
        // ---- filter modal input ------------------------------------------
        if (down & KEY_B) act.kind = SW_ACT_MODAL_CLOSE;

        if (down & (KEY_DOWN | KEY_CSTICK_DOWN)) st->modal_sel++;
        if (down & (KEY_UP   | KEY_CSTICK_UP))   st->modal_sel--;
        if (down & KEY_ZL) st->modal_page = 0;
        if (down & KEY_ZR) st->modal_page = 1;
        modal_follow(st);

        if (down & KEY_A) { act.kind = SW_ACT_FILTER_PICK; act.index = st->modal_sel; }

        touchPosition tp;
        hidTouchRead(&tp);

        if (down & KEY_TOUCH) {
            s_touching = true;
            s_dragged  = false;
            s_touch_x0 = s_touch_x = tp.px;
            s_touch_y0 = s_touch_y = tp.py;
        } else if ((held & KEY_TOUCH) && s_touching) {
            int dy = tp.py - s_touch_y;
            if (dy > 6 || dy < -6 || s_touch_y0 - tp.py > 6 || tp.py - s_touch_y0 > 6) {
                int rows = (s_touch_y - tp.py) / (int)(SW_MODAL_ROW_H / 2);
                if (rows) {
                    st->modal_scroll += rows;
                    int max_scroll = st->row_count - SW_MODAL_ROWS_VISIBLE;
                    if (max_scroll < 0) max_scroll = 0;
                    if (st->modal_scroll > max_scroll) st->modal_scroll = max_scroll;
                    if (st->modal_scroll < 0) st->modal_scroll = 0;
                    s_touch_y = tp.py;
                    s_dragged = true;
                }
            }
            s_touch_x = tp.px;
        } else if ((up & KEY_TOUCH) && s_touching) {
            s_touching = false;

            if (!s_dragged) {
                // Geometry has to match draw_modal() exactly - both read the
                // same SW_MODAL_* macros, so they cannot drift apart.
                float px = SW_MODAL_X, py = SW_MODAL_Y, pw = SW_MODAL_W;
                float seg_y1  = py + SW_MODAL_TITLE_H + SW_MODAL_SEG_H;
                float list_y1 = seg_y1 + SW_MODAL_ROWS_VISIBLE * SW_MODAL_ROW_H;
                float foot_y1 = list_y1 + SW_MODAL_FOOTER_H;

                float seg_y0 = py + SW_MODAL_TITLE_H;

                if (s_touch_x0 >= px && s_touch_x0 < px + pw &&
                    s_touch_y0 >= py && s_touch_y0 < py + SW_MODAL_H) {
                    if (s_touch_y0 < seg_y0) {
                        // The title band. Deliberately inert - it used to fall
                        // into the segmented control's branch below, because
                        // that branch tested only the control's END edge, so
                        // tapping the words "Filter stations" silently switched
                        // between Country and Bitrate. A label is not a button.
                    } else if (s_touch_y0 < seg_y1) {
                        st->modal_page = (s_touch_x0 < px + pw / 2) ? 0 : 1;
                    } else if (s_touch_y0 < list_y1) {
                        int v = (int)((s_touch_y0 - seg_y1) / SW_MODAL_ROW_H);
                        int i = st->modal_scroll + v;
                        if (i >= 0 && i < st->row_count) {
                            st->modal_sel = i;
                            act.kind  = SW_ACT_FILTER_PICK;
                            act.index = i;
                        }
                    } else if (s_touch_y0 < foot_y1) {
                        act.kind = (s_touch_x0 < px + pw / 2) ? SW_ACT_FILTER_APPLY
                                                                : SW_ACT_FILTER_CLEAR;
                    }
                }
            }
        }
    } else {
        // Tabs. L and R would be the obvious shoulder mapping, but they are
        // volume here - volume is adjusted constantly and tabs rarely, so the
        // buttons that can be found without looking go to the frequent job.
        if (down & KEY_ZL) { st->tab = (st->tab + SW_TAB_COUNT - 1) % SW_TAB_COUNT; act.kind = SW_ACT_TAB; }
        if (down & KEY_ZR) { st->tab = (st->tab + 1) % SW_TAB_COUNT;                act.kind = SW_ACT_TAB; }

        if (down & KEY_L) act.kind = SW_ACT_VOL_DOWN;
        if (down & KEY_R) act.kind = SW_ACT_VOL_UP;

        if (down & (KEY_DOWN  | KEY_CSTICK_DOWN)) st->selected++;
        if (down & (KEY_UP    | KEY_CSTICK_UP))   st->selected--;
        if (down & KEY_RIGHT) st->selected += SW_ROWS_VISIBLE;
        if (down & KEY_LEFT)  st->selected -= SW_ROWS_VISIBLE;
        follow_selection(st);

        if (down & KEY_A) { act.kind = SW_ACT_SELECT; act.index = st->selected; }
        if (down & KEY_Y) { act.kind = SW_ACT_STAR;   act.index = st->selected; }
        if (down & KEY_X)   act.kind = SW_ACT_STOP;
        if (down & KEY_START) act.kind = SW_ACT_QUIT;

        // SELECT re-opens the keyboard on the search tab (item 1); B opens
        // the filter modal (item 2). Both are otherwise-unused buttons - see
        // the button-budget note in the item writeup for why L/R/ZL/ZR/A/Y/X/
        // START were all already spoken for.
        if (down & KEY_SELECT) act.kind = SW_ACT_SEARCH;
        if (down & KEY_B)      act.kind = SW_ACT_FILTER_OPEN;

        // ---- touch ------------------------------------------------------
        touchPosition tp;
        hidTouchRead(&tp);

        if (down & KEY_TOUCH) {
            s_touching = true;
            s_dragged  = false;
            s_touch_x0 = s_touch_x = tp.px;
            s_touch_y0 = s_touch_y = tp.py;
        } else if ((held & KEY_TOUCH) && s_touching) {
            int dy = tp.py - s_touch_y;
            if (dy > 6 || dy < -6 || s_touch_y0 - tp.py > 6 || tp.py - s_touch_y0 > 6) {
                // Dragging scrolls whole rows. Sub-row smoothing would be nicer
                // but needs the drag to be tracked against a float offset, and
                // a list of five rows does not earn the complexity.
                int rows = (s_touch_y - tp.py) / (SW_ROW_H / 2);
                if (rows) {
                    st->scroll += rows;
                    clamp_scroll(st);
                    s_touch_y = tp.py;
                    s_dragged = true;
                }
            }
            s_touch_x = tp.px;
        } else if ((up & KEY_TOUCH) && s_touching) {
            s_touching = false;

            if (!s_dragged) {
                if (s_touch_y0 < SW_TAB_H) {
                    int t = s_touch_x0 / (SW_BOT_W / SW_TAB_COUNT);
                    if (t >= 0 && t < SW_TAB_COUNT && t != (int)st->tab) {
                        st->tab  = (SwUiTab)t;
                        act.kind = SW_ACT_TAB;
                    }
                } else if (st->show_search_bar && s_touch_y0 < SW_LIST_Y) {
                    // The head strip's y range [SW_TAB_H, SW_LIST_Y) used to be
                    // dead space - any touch there was swallowed with no
                    // effect. That is exactly where the search bar and filter
                    // chip now live.
                    if (s_touch_x0 >= SW_BOT_W - 40) {
                        act.kind = SW_ACT_FILTER_OPEN;
                    } else {
                        act.kind = SW_ACT_SEARCH;
                    }
                } else {
                    int i = row_at(st, s_touch_x0, s_touch_y0);
                    if (i >= 0) {
                        st->selected = i;
                        follow_selection(st);
                        act.kind  = SW_ACT_SELECT;
                        act.index = i;
                        s_flash_row = i;
                        s_flash_ttl = 8;
                    }
                }
            }
        }
    }

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    draw_top(st);
    draw_bottom(st);
    C3D_FrameEnd(0);

    // Cleared after drawing, not before: the C2D_Text values built this frame
    // point into this buffer and are still being read until FrameEnd returns.
    C2D_TextBufClear(s_buf);

    return act;
}
