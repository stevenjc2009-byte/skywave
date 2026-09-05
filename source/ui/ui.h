#pragma once

// The whole interface: a now-playing panel on the top screen and a tabbed,
// scrollable list on the bottom.
//
// It is one widget rather than a screen per mode because every mode is the same
// shape - a list of stations you pick one from. Browsing, favourites and search
// results differ only in what fills `rows`, so they share the drawing, the
// scrolling and the touch handling instead of each reimplementing them.
//
// This file knows nothing about playback or the directory. It is handed strings
// and hands back what the user pressed; app.c is what joins the two. That split
// is what lets the UI be changed without any risk to the audio path.

#include <stdbool.h>

#define UI_MAX_ROWS   40
#define UI_ROW_TITLE  72
#define UI_ROW_SUB    72

typedef enum {
    SW_TAB_BROWSE = 0,
    SW_TAB_FAVOURITES,
    SW_TAB_SEARCH,
    SW_TAB_ABOUT,
    SW_TAB_COUNT
} SwUiTab;

typedef struct {
    char title[UI_ROW_TITLE];
    char sub[UI_ROW_SUB];
    bool starred;       // draws the favourite marker
    bool playing;       // draws the row as the one currently on air
} SwUiRow;

typedef struct {
    // ---- top screen -----------------------------------------------------
    const char *station;      // "" when nothing is loaded
    const char *now;          // now-playing text from the stream, or ""
    const char *status;       // "Playing", "Buffering...", and so on
    const char *message;      // an error or a notice; NULL when there is none
    bool        message_bad;  // true colours the message as a problem
    int         buffer_pct;   // 0..100, or -1 to hide the bar entirely
    int         volume;       // 0..100

    // ---- bottom screen --------------------------------------------------
    SwUiTab     tab;
    const char *list_title;
    const char *empty_text;   // shown in place of an empty list
    SwUiRow     rows[UI_MAX_ROWS];
    int         row_count;
    int         selected;     // swUiFrame keeps this in range and on screen
    int         scroll;       // first visible row; swUiFrame maintains it
    bool        busy;         // draws a working indicator and ignores input
} SwUi;

typedef enum {
    SW_ACT_NONE = 0,
    SW_ACT_SELECT,     // A, or a touch released on a row; `index` says which
    SW_ACT_STAR,       // Y on `index`
    SW_ACT_STOP,       // X
    SW_ACT_TAB,        // the tab changed; read st->tab
    SW_ACT_VOL_UP,
    SW_ACT_VOL_DOWN,
    SW_ACT_QUIT        // START, or the console asked the app to close
} SwUiActionKind;

typedef struct {
    SwUiActionKind kind;
    int            index;
} SwUiAction;

bool swUiInit(void);
void swUiExit(void);

// Draws one frame and reads the pad. Returns what the user did, if anything.
// Blocks for vblank, so calling this is also what paces the main loop.
SwUiAction swUiFrame(SwUi *st);

// True until the console asks the app to close. The main loop's condition.
bool swUiRunning(void);
