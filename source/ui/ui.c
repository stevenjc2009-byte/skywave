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
    s_buf = C2D_TextBufNew(4096);

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

    // Header strip.
    C2D_DrawRectSolid(0, 0, 0.0f, SW_TOP_W, 26, SW_PANEL);
    C2D_DrawRectSolid(0, 26, 0.0f, SW_TOP_W, 1, SW_LINE);
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
        // Station name, large, because it is the one thing worth reading from
        // across a room.
        text(20, 52, 0.85f, SW_TEXT, SW_TOP_W - 40, st->station);

        if (st->now && st->now[0])
            text(20, 88, 0.52f, SW_ACCENT, SW_TOP_W - 40, st->now);
        else
            text(20, 88, 0.52f, SW_TEXT_FAINT, SW_TOP_W - 40,
                 "This station sends no track information.");

        text(20, 118, 0.50f, SW_TEXT_DIM, SW_TOP_W - 40, st->status);
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
    float w = (float)SW_BOT_W / SW_TAB_COUNT;

    C2D_DrawRectSolid(0, 0, 0.0f, SW_BOT_W, SW_TAB_H, SW_PANEL);

    for (int i = 0; i < SW_TAB_COUNT; i++) {
        bool on = (int)st->tab == i;
        if (on) {
            C2D_DrawRectSolid(w * i, 0, 0.0f, w, SW_TAB_H, SW_PANEL_HI);
            C2D_DrawRectSolid(w * i, SW_TAB_H - 3, 0.0f, w, 3, SW_ACCENT);
        }
        text_centre(w * i, 6, w, 0.48f, on ? SW_TEXT : SW_TEXT_DIM, kTabNames[i]);
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
        bool sel = (i == st->selected);

        if (sel) C2D_DrawRectSolid(0, y, 0.0f, SW_BOT_W, SW_ROW_H - 2, SW_PANEL_HI);

        // A stripe down the left edge marks the station that is on air. It is
        // the fastest way to answer "am I looking at what I am hearing?".
        if (r->playing) C2D_DrawRectSolid(0, y, 0.0f, 4, SW_ROW_H - 2, SW_ACCENT);

        float text_w = SW_BOT_W - 24 - (r->starred ? 18 : 0);
        text(12, y + 3,  0.50f, r->playing ? SW_ACCENT : SW_TEXT,     text_w, r->title);
        text(12, y + 19, 0.40f, SW_TEXT_DIM, text_w, r->sub);

        // An amber dot for a saved station, drawn rather than typed: the
        // console's system font has no star, and a missing glyph renders as a
        // blank box that reads as a bug.
        if (r->starred)
            C2D_DrawCircleSolid(SW_BOT_W - 14, y + SW_ROW_H / 2 - 1, 0.0f, 5, SW_ACCENT);
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

static void draw_footer(const SwUi *st)
{
    float y = SW_SCREEN_H - SW_FOOTER_H;
    C2D_DrawRectSolid(0, y, 0.0f, SW_BOT_W, SW_FOOTER_H, SW_PANEL);
    C2D_DrawRectSolid(0, y, 0.0f, SW_BOT_W, 1, SW_LINE);

    const char *hint;
    switch (st->tab) {
        case SW_TAB_SEARCH:  hint = "A: search   X: stop   START: exit"; break;
        case SW_TAB_ABOUT:   hint = "A: choose   START: exit";           break;
        default:             hint = "A: play   Y: save   X: stop";       break;
    }
    text(8, y + 6, 0.42f, SW_TEXT_DIM, SW_BOT_W - 16, hint);
}

static void draw_bottom(const SwUi *st)
{
    C2D_TargetClear(s_bot, SW_BG);
    C2D_SceneBegin(s_bot);

    draw_tabs(st);

    if (st->list_title && st->list_title[0])
        text(8, SW_HEAD_Y, 0.42f, SW_TEXT_FAINT, SW_BOT_W - 16, st->list_title);

    draw_rows(st);
    draw_footer(st);

    if (st->busy) {
        // A flat dim sheet over the list, so it is obvious that touching it
        // will do nothing rather than the app merely feeling unresponsive.
        C2D_DrawRectSolid(0, SW_LIST_Y, 0.0f, SW_BOT_W, SW_LIST_H,
                          C2D_Color32(0x0C, 0x14, 0x24, 0xC0));
        text_centre(0, SW_LIST_Y + 70, SW_BOT_W, 0.55f, SW_ACCENT, "Working...");
    }
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

    follow_selection(st);

    if (!st->busy) {
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
                } else {
                    int i = row_at(st, s_touch_x0, s_touch_y0);
                    if (i >= 0) {
                        st->selected = i;
                        follow_selection(st);
                        act.kind  = SW_ACT_SELECT;
                        act.index = i;
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
