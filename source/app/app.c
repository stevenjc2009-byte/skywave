#include "app.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../audio/player.h"
#include "../net/directory.h"
#include "../store/favourites.h"
#include "../ui/ui.h"
#include "../update/update.h"
#include "../version.h"

// Anything that talks to the network takes between a moment and several seconds
// and cannot be hurried. Doing it on the main thread would freeze the screen for
// the whole of it, which on a handheld reads as a crash. So every one of them
// runs on a worker and the UI keeps drawing with a "Working..." sheet over the
// list - the app is visibly busy rather than apparently dead.
typedef enum {
    JOB_NONE = 0,
    JOB_TOP,
    JOB_SEARCH,
    JOB_UPDATE
} JobKind;

typedef struct {
    SwUi ui;

    SwStationList browse;      // the popular list, fetched once per session
    SwStationList results;     // the last search
    bool          browse_ready;
    bool          results_ready;

    char search_q[64];

    // Worker
    Thread        worker;
    volatile bool job_running;
    JobKind       job_kind;
    SwStationList job_out;
    SwDirResult   job_dir_result;
    sw_update_t   job_update_result;
    char          job_update_tag[32];

    char message[96];
    bool message_bad;
    bool update_installed;     // a restart is pending; changes the exit path

    char now[256];
    char status[64];
} App;

static App g;

// ------------------------------------------------------------------ messages

static void say(bool bad, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(bool bad, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.message, sizeof(g.message), fmt, ap);
    va_end(ap);
    g.message_bad = bad;
}

// --------------------------------------------------------------- the worker

static void worker_main(void *arg)
{
    (void)arg;

    switch (g.job_kind) {
        case JOB_TOP:
            g.job_dir_result = swDirTopStations(&g.job_out);
            break;
        case JOB_SEARCH:
            g.job_dir_result = swDirSearchByName(g.search_q, &g.job_out);
            break;
        case JOB_UPDATE:
            g.job_update_result = sw_update_run(g.job_update_tag, sizeof(g.job_update_tag));
            break;
        default:
            break;
    }

    g.job_running = false;
}

static void job_start(JobKind kind)
{
    if (g.job_running) return;

    // The previous worker has finished but its handle is still ours to free.
    if (g.worker) { threadJoin(g.worker, U64_MAX); threadFree(g.worker); g.worker = NULL; }

    g.job_kind    = kind;
    g.job_running = true;
    g.ui.busy     = true;

    // Core 1, like the network thread in the player and for the same reason:
    // this blocks on the network for seconds at a time and has no business
    // competing with the decoder or the UI for core 0.
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g.worker = threadCreate(worker_main, NULL, 24 * 1024, prio, 1, false);

    // Same fallback as the player's network thread: core 1 may not be ours to
    // use, and a search that runs a little less smoothly beats one that refuses.
    if (!g.worker)
        g.worker = threadCreate(worker_main, NULL, 24 * 1024, prio, 0, false);

    if (!g.worker) {
        g.job_running = false;
        g.ui.busy     = false;
        say(true, "Could not start a background task.");
    }
}

static void job_finish(void)
{
    g.ui.busy = false;

    switch (g.job_kind) {
        case JOB_TOP:
            if (g.job_dir_result == SW_DIR_OK) {
                g.browse       = g.job_out;
                g.browse_ready = true;
                say(false, "Loaded %d stations.", g.browse.count);
            } else {
                say(true, "%s", swDirErrorText(g.job_dir_result));
            }
            break;

        case JOB_SEARCH:
            g.results       = g.job_out;
            g.results_ready = true;
            if (g.job_dir_result == SW_DIR_OK)
                say(false, "%d results for \"%s\".", g.results.count, g.search_q);
            else
                say(true, "%s", swDirErrorText(g.job_dir_result));
            break;

        case JOB_UPDATE:
            say(g.job_update_result != SW_UPDATE_INSTALLED &&
                g.job_update_result != SW_UPDATE_CURRENT,
                "%s", sw_update_str(g.job_update_result));

            if (g.job_update_result == SW_UPDATE_INSTALLED) {
                g.update_installed = true;
                // Nothing restarts here. Chainloading has to happen during a
                // normal shutdown, so the exit path in swAppRun arms it after
                // the audio and the graphics have been torn down properly.
                say(false, "Updated to v%s. Press START to restart.", g.job_update_tag);
            }
            break;

        default:
            break;
    }

    g.job_kind = JOB_NONE;
}

// ------------------------------------------------------------------- lists

// Which station list the current tab is showing, or NULL for a tab that is not
// a list of stations.
static const SwStationList *active_list(void)
{
    switch (g.ui.tab) {
        case SW_TAB_BROWSE:     return &g.browse;
        case SW_TAB_FAVOURITES: return swFavList();
        case SW_TAB_SEARCH:     return &g.results;
        default:                return NULL;
    }
}

static void fill_about_rows(void)
{
    int n = 0;

    snprintf(g.ui.rows[n].title, UI_ROW_TITLE, "Check for updates");
    snprintf(g.ui.rows[n].sub,   UI_ROW_SUB,   "Downloads and installs the newest release");
    n++;

    snprintf(g.ui.rows[n].title, UI_ROW_TITLE, "Skywave v%s", SKYWAVE_VERSION);
    snprintf(g.ui.rows[n].sub,   UI_ROW_SUB,   "Internet radio for the Nintendo 3DS");
    n++;

    snprintf(g.ui.rows[n].title, UI_ROW_TITLE, "Stations by radio-browser.info");
    snprintf(g.ui.rows[n].sub,   UI_ROW_SUB,   "A free, community-maintained station index");
    n++;

    snprintf(g.ui.rows[n].title, UI_ROW_TITLE, "Sound needs dspfirm.cdc");
    snprintf(g.ui.rows[n].sub,   UI_ROW_SUB,   "Dumped from your own console with DSP1");
    n++;

    g.ui.row_count = n;
}

static void fill_station_rows(const SwStationList *l)
{
    const SwStation *live = swPlayerStation();

    g.ui.row_count = l->count > UI_MAX_ROWS ? UI_MAX_ROWS : l->count;

    for (int i = 0; i < g.ui.row_count; i++) {
        const SwStation *s = &l->items[i];
        SwUiRow *r = &g.ui.rows[i];

        snprintf(r->title, UI_ROW_TITLE, "%s", s->name);

        if (s->country[0] && s->bitrate > 0)
            snprintf(r->sub, UI_ROW_SUB, "%s  -  %d kbps", s->country, s->bitrate);
        else if (s->bitrate > 0)
            snprintf(r->sub, UI_ROW_SUB, "%d kbps", s->bitrate);
        else
            snprintf(r->sub, UI_ROW_SUB, "%s", s->country);

        r->starred = swFavContains(s);
        r->playing = live && strcmp(live->url, s->url) == 0;
    }
}

static void rebuild_rows(void)
{
    memset(g.ui.rows, 0, sizeof(g.ui.rows));

    if (g.ui.tab == SW_TAB_ABOUT) {
        g.ui.list_title = "About Skywave";
        g.ui.empty_text = NULL;
        fill_about_rows();
        return;
    }

    const SwStationList *l = active_list();

    switch (g.ui.tab) {
        case SW_TAB_BROWSE:
            g.ui.list_title = "Most listened to";
            g.ui.empty_text = g.job_running ? "Loading stations..."
                                            : "No stations loaded. Press A to retry.";
            break;
        case SW_TAB_FAVOURITES:
            g.ui.list_title = "Saved stations";
            g.ui.empty_text = "Press Y on any station to save it here.";
            break;
        case SW_TAB_SEARCH:
            g.ui.list_title = g.search_q[0] ? g.search_q : "Search";
            g.ui.empty_text = "Press A to type a station name.";
            break;
        default:
            break;
    }

    if (l) fill_station_rows(l);
}

// ------------------------------------------------------------------ actions

// Opens the console's own keyboard. It is a system applet, so it blocks and
// takes over both screens - which is fine here and much better than trying to
// draw a keyboard onto a 320-pixel screen that already has a list on it.
static bool ask_for_text(char *dst, size_t cap, const char *hint)
{
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)cap - 1);
    swkbdSetHintText(&kb, hint);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&kb, SWKBD_PREDICTIVE_INPUT);

    return swkbdInputText(&kb, dst, cap) == SWKBD_BUTTON_CONFIRM;
}

static void play_index(int i)
{
    const SwStationList *l = active_list();
    if (!l || i < 0 || i >= l->count) return;

    if (!swPlayerPlay(&l->items[i]))
        say(true, "%s", swPlayerError()[0] ? swPlayerError() : "Could not start playback.");
    else
        say(false, "Tuning in to %s...", l->items[i].name);
}

static void star_index(int i)
{
    const SwStationList *l = active_list();
    if (!l || i < 0 || i >= l->count) return;

    SwStation st = l->items[i];   // copied: removing from favourites moves the list

    if (swFavToggle(&st)) say(false, "Saved %s.", st.name);
    else                  say(false, "Removed %s.", st.name);
}

static void handle(SwUiAction a)
{
    switch (a.kind) {
        case SW_ACT_SELECT:
            if (g.ui.tab == SW_TAB_ABOUT) {
                if (a.index == 0) {
                    // Installing a title while decoding audio is asking for
                    // trouble on a console this size, and the update ends in a
                    // restart anyway - so stop first.
                    swPlayerStop();
                    job_start(JOB_UPDATE);
                }
            } else if (g.ui.tab == SW_TAB_SEARCH && g.ui.row_count == 0) {
                if (ask_for_text(g.search_q, sizeof(g.search_q), "Station name"))
                    job_start(JOB_SEARCH);
            } else if (g.ui.tab == SW_TAB_BROWSE && g.ui.row_count == 0) {
                job_start(JOB_TOP);
            } else {
                play_index(a.index);
            }
            break;

        case SW_ACT_STAR:
            if (g.ui.tab != SW_TAB_ABOUT) star_index(a.index);
            break;

        case SW_ACT_STOP:
            swPlayerStop();
            say(false, "Stopped.");
            break;

        case SW_ACT_TAB:
            g.ui.selected = 0;
            g.ui.scroll   = 0;

            // Fetch on first arrival rather than at startup: a user who opens
            // the app to resume a saved station should not wait on a list they
            // are not going to look at.
            if (g.ui.tab == SW_TAB_BROWSE && !g.browse_ready && !g.job_running)
                job_start(JOB_TOP);
            if (g.ui.tab == SW_TAB_SEARCH && !g.results_ready && !g.search_q[0]) {
                if (ask_for_text(g.search_q, sizeof(g.search_q), "Station name"))
                    job_start(JOB_SEARCH);
            }
            break;

        case SW_ACT_VOL_UP:   swPlayerSetVolume(swPlayerVolume() + 5); break;
        case SW_ACT_VOL_DOWN: swPlayerSetVolume(swPlayerVolume() - 5); break;

        default:
            break;
    }
}

// -------------------------------------------------------------- the top half

static void refresh_top_screen(void)
{
    const SwStation *s = swPlayerStation();

    g.ui.station = s ? s->name : "";
    swPlayerNowPlaying(g.now, sizeof(g.now));
    g.ui.now = g.now;

    switch (swPlayerState()) {
        case SW_PLAY_CONNECTING:
            snprintf(g.status, sizeof(g.status), "Connecting...");
            g.ui.buffer_pct = -1;
            break;
        case SW_PLAY_BUFFERING:
            snprintf(g.status, sizeof(g.status), "Buffering...");
            g.ui.buffer_pct = swPlayerBufferPercent();
            break;
        case SW_PLAY_PLAYING:
            snprintf(g.status, sizeof(g.status), "Playing");
            g.ui.buffer_pct = swPlayerBufferPercent();
            break;
        case SW_PLAY_ERROR:
            snprintf(g.status, sizeof(g.status), "Stopped");
            g.ui.buffer_pct = -1;
            // The player's own message is more specific than anything here
            // could be, so it wins over whatever the last action said.
            if (swPlayerError()[0]) say(true, "%s", swPlayerError());
            break;
        default:
            snprintf(g.status, sizeof(g.status), "Stopped");
            g.ui.buffer_pct = -1;
            break;
    }

    g.ui.status      = g.status;
    g.ui.volume      = swPlayerVolume();
    g.ui.message     = g.message[0] ? g.message : NULL;
    g.ui.message_bad = g.message_bad;
}

// ------------------------------------------------------------------ the loop

void swAppRun(void)
{
    memset(&g, 0, sizeof(g));

    swDirInit();
    swFavLoad();

    // Open on saved stations when there are any. Someone who has been here
    // before almost always wants one of their own stations, and showing them
    // first means the common case needs no network wait at all.
    g.ui.tab = swFavList()->count > 0 ? SW_TAB_FAVOURITES : SW_TAB_BROWSE;
    if (g.ui.tab == SW_TAB_BROWSE) job_start(JOB_TOP);

    while (swUiRunning()) {
        if (g.ui.busy && !g.job_running) job_finish();

        refresh_top_screen();
        rebuild_rows();

        SwUiAction a = swUiFrame(&g.ui);
        if (a.kind == SW_ACT_QUIT) break;
        handle(a);
    }

    swPlayerStop();

    if (g.worker) { threadJoin(g.worker, U64_MAX); threadFree(g.worker); g.worker = NULL; }

    // Arm the restart last, once nothing is still running. The jump itself
    // happens as the process exits.
    if (g.update_installed) sw_update_relaunch();
}
