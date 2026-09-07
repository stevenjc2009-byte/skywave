#include "app.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../audio/player.h"
#include "../net/directory.h"
#include "../net/http.h"
#include "../net/tcp.h"
#include "../store/diag.h"
#include "../store/favourites.h"
#include "../store/presets.h"
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
    bool          browse_is_presets;  // browse currently shows the built-in
                                      // list, not a live directory answer

    char search_q[64];

    // The server-side filter. `filter` is what the last search actually ran
    // with; `filter_draft` is what the modal is editing and is only copied
    // into `filter` on Apply, so backing out of the modal changes nothing.
    SwDirFilter filter;
    SwDirFilter filter_draft;
    char        filter_label[128];  // built by filter_summary(); backs ui.list_title

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
    SwPlayerInitResult audio;  // SW_PLAYER_OK, or why there is no sound

    char now[256];
    char status[64];

    // The startup update check. A dedicated thread rather than a JOB_* kind:
    // routing it through the job worker would either drop it on the floor
    // (job_running already true from JOB_TOP at boot) or grey out the whole
    // list via g.ui.busy for a check the user did not ask for and does not
    // need to wait on.
    Thread        upd_thr;
    volatile bool upd_running;
    bool          upd_started;    // the check has been kicked off once
    bool          upd_available;  // a newer release exists; durable badge/row
    sw_update_t   upd_result;
    char          upd_tag[32];

    // The startup check's own SwHttp, so it can be cancelled from the main
    // thread the same way player.c's g.ping is: a member of the app's global
    // state rather than a local on update_check_main's stack, because SwHttp
    // is 17,584 bytes and that thread gets a 16 KB stack (see
    // sw_update_check_h in update.h). Living here is also what lets the exit
    // path in swAppRun reach it to cancel it before joining upd_thr - see
    // that join for the hang this closes.
    SwHttp        upd_http;
    int           frame;          // counts main-loop iterations, for timing
} App;

static App g;

// Set to -1 whenever the filter modal is closed; set to `modal_page` the
// instant fill_filter_modal_rows() notices the modal just opened or the page
// changed. That is the one moment `modal_sel`/`modal_scroll` get resynced
// from filter_draft - every other frame leaves them alone so they do not
// fight the user's own up/down navigation.
static int s_modal_state = -1;

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
            // g.filter.name is synced from g.search_q by start_search() on
            // the main thread before this job is started, not here - the
            // worker only ever reads g.filter, never writes it, so there is
            // nothing for the main thread to race.
            g.job_dir_result = swDirSearch(&g.filter, &g.job_out);
            break;
        case JOB_UPDATE:
            g.job_update_result = sw_update_run(g.job_update_tag, sizeof(g.job_update_tag));
            break;
        default:
            break;
    }

    // `volatile` only stops the compiler reordering this store ahead of the
    // ~19 KB of job_out/job_dir_result writes above - the MPCore's memory
    // system can still let core 0 observe job_running go false before those
    // writes land. The barrier makes the payload visible before the flag that
    // says it is safe to read.
    __dmb();
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

// Every place that starts a search goes through here rather than calling
// job_start(JOB_SEARCH) directly, so the filter's free-text field always
// matches what is actually on screen in g.search_q before the worker reads
// it.
static void start_search(void)
{
    snprintf(g.filter.name, sizeof(g.filter.name), "%s", g.search_q);
    job_start(JOB_SEARCH);
}

static void job_finish(void)
{
    // Pairs with the __dmb() the worker does before clearing job_running: that
    // barrier is only worth anything if this side also has one, so that the
    // job_out/job_dir_result reads below cannot be hoisted ahead of the
    // job_running read that gated calling this function.
    __dmb();
    g.ui.busy = false;

    switch (g.job_kind) {
        case JOB_TOP:
            if (g.job_dir_result == SW_DIR_OK) {
                g.browse           = g.job_out;
                g.browse_ready     = true;
                g.browse_is_presets = false;
                say(false, "Loaded %d stations.", g.browse.count);
            } else {
                // Leave g.browse exactly as it was. If it is still the preset
                // seed from startup, a wifi-off user keeps something to play
                // instead of an empty list - see swAppRun's seeding block.
                say(true, "Using built-in stations - %s", swDirErrorText(g.job_dir_result));
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

    // The startup check (see start_update_check/finish_update_check) only
    // ever sets g.upd_available true on SW_UPDATE_AVAILABLE - never on
    // SW_UPDATE_CURRENT or an error - so this row only changes shape when
    // there really is something to install.
    if (g.upd_available) {
        snprintf(g.ui.rows[n].title, UI_ROW_TITLE, "Version %s is available", g.upd_tag);
        snprintf(g.ui.rows[n].sub,   UI_ROW_SUB,   "Press A to install");
    } else {
        snprintf(g.ui.rows[n].title, UI_ROW_TITLE, "Check for updates");
        snprintf(g.ui.rows[n].sub,   UI_ROW_SUB,   "Downloads and installs the newest release");
    }
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

    // Safe to ask every frame: swNetTrustStoreSource() answers from a one-off
    // look at the two bundle paths, not from the 121-certificate parse, and it
    // does not wait on that parse if one is running. The question the row exists
    // to answer is whether the user's sdmc:/config/ssl/cacert.pem override is
    // the one in use, so it has to be answerable before an update check has
    // happened to force the load.
    snprintf(g.ui.rows[n].title, UI_ROW_TITLE, "Certificate bundle");
    snprintf(g.ui.rows[n].sub,   UI_ROW_SUB,   "%s", swNetTrustStoreSource());
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

        // A bitrate is only worth printing if it could be true. The directory
        // is community-editable, and json_int() now saturates rather than
        // overflowing, so a station claiming thirty nines arrives here as
        // INT_MAX and used to render as "2147483647 kbps". Anything above the
        // sane ceiling is treated as not knowing the bitrate at all, which is
        // what an absurd number actually means - the row falls back to the
        // country alone rather than repeating a lie in a smaller font.
        bool known_rate = s->bitrate > 0 && s->bitrate <= SW_BITRATE_SANE_MAX;

        if (s->country[0] && known_rate)
            snprintf(r->sub, UI_ROW_SUB, "%s  -  %d kbps", s->country, s->bitrate);
        else if (known_rate)
            snprintf(r->sub, UI_ROW_SUB, "%d kbps", s->bitrate);
        else
            snprintf(r->sub, UI_ROW_SUB, "%s", s->country);

        r->starred = swFavContains(s);
        r->playing = live && strcmp(live->url, s->url) == 0;
    }
}

// Fills g.ui.rows with the country list or the bitrate list, whichever
// modal_page is showing, and marks the row matching the filter currently
// being edited by repurposing SwUiRow.playing - inside the modal that field
// means "this is the applied value", not "this station is on air". Only
// resyncs modal_sel/modal_scroll on the frame the modal opens or the page
// changes (tracked by s_modal_state), so it does not fight live navigation.
static void fill_filter_modal_rows(void)
{
    bool changed = (s_modal_state != g.ui.modal_page);
    s_modal_state = g.ui.modal_page;

    if (g.ui.modal_page == 0) {
        int count = 0;
        const SwDirCountry *countries = swDirCountries(&count);
        if (count > UI_MAX_ROWS) count = UI_MAX_ROWS;  // 30 real entries; always true

        int current = swDirCountryIndex(g.filter_draft.countrycode);

        for (int i = 0; i < count; i++) {
            SwUiRow *r = &g.ui.rows[i];
            snprintf(r->title, UI_ROW_TITLE, "%s", countries[i].name);
            r->sub[0]   = '\0';
            r->starred  = false;
            r->playing  = (i == current);
        }
        g.ui.row_count = count;

        if (changed) {
            g.ui.modal_sel    = current >= 0 ? current : 0;
            g.ui.modal_scroll = 0;
        }
    } else {
        for (int b = 0; b < SW_BAND_COUNT; b++) {
            SwUiRow *r = &g.ui.rows[b];
            snprintf(r->title, UI_ROW_TITLE, "%s", swDirBandLabel((SwBitrateBand)b));
            r->sub[0]  = '\0';
            r->starred = false;
            r->playing = (b == (int)g.filter_draft.band);
        }
        g.ui.row_count = SW_BAND_COUNT;

        if (changed) {
            g.ui.modal_sel    = (int)g.filter_draft.band;
            g.ui.modal_scroll = 0;
        }
    }
}

// Builds the search tab's title out of whatever is actually active, e.g.
// "jazz - United Kingdom - 129 - 192 kbps", so the filter's effect is visible
// without opening the modal again. "Search" when nothing is set at all.
static void filter_summary(char *out, size_t cap)
{
    const char *parts[3];
    int n = 0;

    if (g.search_q[0]) parts[n++] = g.search_q;

    const char *country_name = NULL;
    if (g.filter.countrycode[0]) {
        int idx = swDirCountryIndex(g.filter.countrycode);
        if (idx >= 0) country_name = swDirCountries(NULL)[idx].name;
    }
    if (country_name) parts[n++] = country_name;

    const char *band_label = NULL;
    if (g.filter.band != SW_BAND_ANY) band_label = swDirBandLabel(g.filter.band);
    if (band_label) parts[n++] = band_label;

    if (n == 0) {
        snprintf(out, cap, "Search");
        return;
    }

    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s%s", i ? " - " : "", parts[i]);
    }
}

static void rebuild_rows(void)
{
    memset(g.ui.rows, 0, sizeof(g.ui.rows));
    g.ui.tab_badge[SW_TAB_ABOUT] = g.upd_available;

    if (g.ui.modal == SW_MODAL_FILTER) {
        fill_filter_modal_rows();
        return;
    }
    s_modal_state = -1;

    if (g.ui.tab == SW_TAB_ABOUT) {
        g.ui.list_title = "About Skywave";
        g.ui.empty_text = NULL;
        fill_about_rows();
        return;
    }

    const SwStationList *l = active_list();

    switch (g.ui.tab) {
        case SW_TAB_BROWSE:
            g.ui.list_title = g.browse_is_presets ? "Built-in stations" : "Most listened to";
            g.ui.empty_text = g.job_running ? "Loading stations..."
                                            : "No stations loaded. Press A to retry.";
            break;
        case SW_TAB_FAVOURITES:
            g.ui.list_title = "Saved stations";
            g.ui.empty_text = "Press Y on any station to save it here.";
            break;
        case SW_TAB_SEARCH:
            filter_summary(g.filter_label, sizeof(g.filter_label));
            g.ui.list_title = g.filter_label;
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
// Types into a local buffer and only copies to `dst` on Confirm, so Cancel
// truly changes nothing - swkbdInputText writes into its output buffer as the
// user types, not only on Confirm, so writing straight into the caller's
// field used to leave a half-typed query behind after backing out. The
// keyboard opens pre-filled with whatever `dst` already holds, so re-opening
// it to refine a query starts from the last one instead of blank.
static bool ask_for_text(char *dst, size_t cap, const char *hint)
{
    // Every real caller passes sizeof(g.search_q) == 64 here; the clamp just
    // keeps this helper honest if that ever changes instead of overflowing.
    char tmp[64];
    size_t kb_cap = cap < sizeof(tmp) ? cap : sizeof(tmp);
    snprintf(tmp, kb_cap, "%s", dst);

    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)kb_cap - 1);
    swkbdSetHintText(&kb, hint);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&kb, SWKBD_PREDICTIVE_INPUT);
    swkbdSetInitialText(&kb, tmp);

    bool confirmed = swkbdInputText(&kb, tmp, kb_cap) == SWKBD_BUTTON_CONFIRM;
    if (confirmed) snprintf(dst, cap, "%s", tmp);
    return confirmed;
}

static void play_index(int i)
{
    // This was a bare `return`, and it was the one silent exit on the whole
    // press-A path: no state change, no message, so the screen simply did not
    // react. That is indistinguishable from the button not working, which is
    // the hardest kind of bug to get a useful report about. Neither branch
    // should be reachable - the UI only offers rows it was handed - but "should
    // not be reachable" is exactly the claim worth instrumenting rather than
    // trusting, and v1.0.4 went looking for a silent failure that presented as
    // a screen saying "Stopped" with no second line.
    const SwStationList *l = active_list();
    if (!l) {
        swDiagf("play_index(%d): active_list() is NULL, tab=%d", i, (int)g.ui.tab);
        say(true, "This tab has no station list to play from.");
        return;
    }
    if (i < 0 || i >= l->count) {
        swDiagf("play_index(%d): out of range, count=%d", i, l->count);
        say(true, "Station %d is not in a list of %d.", i + 1, l->count);
        return;
    }

    swDiagf("play_index(%d) '%s' bitrate=%d url=%s",
            i, l->items[i].name, l->items[i].bitrate, l->items[i].url);

    // Without a working DSP there is nothing to play into, and swPlayerPlay
    // would refuse with a generic message. Say the actual reason instead - it
    // is the one failure here a user can do something about.
    if (g.audio != SW_PLAYER_OK) {
        swDiagf("play_index: refused, audio init was %d", (int)g.audio);
        say(true, "%s", swPlayerInitTextShort(g.audio));
        return;
    }

    if (!swPlayerPlay(&l->items[i])) {
        // swPlayerError() now copies under player's text_lock rather than
        // handing back a raw pointer into memory the network thread writes -
        // so it needs a local buffer instead of two free-standing calls.
        char err[96];
        swPlayerError(err, sizeof(err));
        say(true, "%s", err[0] ? err : "Could not start playback.");
    } else
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
                    // restart anyway - so stop first. This is the same
                    // job_start(JOB_UPDATE) whether the row says "Check for
                    // updates" or "Version x.y.z is available" - sw_update_run
                    // checks and installs in one call either way, so the row
                    // text is the only thing that needs to know which is true.
                    swPlayerStop();
                    job_start(JOB_UPDATE);
                }
            } else if (g.ui.tab == SW_TAB_SEARCH && g.ui.row_count == 0) {
                if (ask_for_text(g.search_q, sizeof(g.search_q), "Station name")) {
                    start_search();
                    // The swkbd applet's own confirming A press can be read
                    // as this app's next A on the very next frame, which
                    // would otherwise play row 0 unasked.
                    g.ui.selected      = 0;
                    g.ui.swallow_frame = true;
                }
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
            g.ui.selected        = 0;
            g.ui.scroll          = 0;
            g.ui.show_search_bar = (g.ui.tab == SW_TAB_SEARCH);

            // Fetch on first arrival rather than at startup: a user who opens
            // the app to resume a saved station should not wait on a list they
            // are not going to look at.
            if (g.ui.tab == SW_TAB_BROWSE && !g.browse_ready && !g.job_running)
                job_start(JOB_TOP);
            if (g.ui.tab == SW_TAB_SEARCH && !g.results_ready && !g.search_q[0]) {
                if (ask_for_text(g.search_q, sizeof(g.search_q), "Station name")) {
                    start_search();
                    g.ui.selected      = 0;
                    g.ui.swallow_frame = true;
                }
            }
            break;

        case SW_ACT_VOL_UP:   swPlayerSetVolume(swPlayerVolume() + 5); break;
        case SW_ACT_VOL_DOWN: swPlayerSetVolume(swPlayerVolume() - 5); break;

        // SELECT on the search tab, whether or not results already exist -
        // this is what makes re-searching from a results screen possible at
        // all. Guarded on job_running first: the busy sheet already blocks
        // this action from being generated in the ordinary case, but there is
        // no reason for the keyboard to fight a search that is mid-flight if
        // it somehow lands on the same frame the job starts.
        case SW_ACT_SEARCH:
            if (g.job_running) break;
            if (g.ui.tab != SW_TAB_SEARCH) break;
            if (ask_for_text(g.search_q, sizeof(g.search_q), "Station name")) {
                start_search();
                g.ui.selected      = 0;
                g.ui.swallow_frame = true;
            }
            break;

        case SW_ACT_FILTER_OPEN:
            if (g.job_running) break;
            if (g.ui.tab != SW_TAB_SEARCH) break;
            g.filter_draft  = g.filter;   // edit a copy; Apply commits it
            g.ui.modal      = SW_MODAL_FILTER;
            g.ui.modal_page = 0;
            break;

        case SW_ACT_FILTER_PICK:
            if (g.ui.modal_page == 0) {
                int count = 0;
                const SwDirCountry *countries = swDirCountries(&count);
                if (a.index >= 0 && a.index < count)
                    snprintf(g.filter_draft.countrycode, sizeof(g.filter_draft.countrycode),
                             "%s", countries[a.index].code);
            } else if (a.index >= 0 && a.index < SW_BAND_COUNT) {
                g.filter_draft.band = (SwBitrateBand)a.index;
            }
            break;

        // Both of the next two commit to g.filter - the very struct the worker
        // thread reads through swDirSearch(&g.filter, ...) while a job is in
        // flight - and then call start_search(), which writes g.filter.name
        // BEFORE job_start() gets as far as its own g.job_running check. So if
        // either of them could run during a job, it would rewrite the filter
        // under a search already using it and job_start would then decline to
        // start the search that was actually asked for: the results would
        // answer neither query.
        //
        // Traced, and it cannot happen today: g.ui.busy and g.job_running are
        // both written on this thread by job_start(), and job_finish() only
        // clears busy once job_running is already false, so busy covers every
        // moment job_running is true - and swUiFrame refuses to emit any action
        // but Quit while busy. The guard is here because that is a two-variable
        // argument spread across three functions, and one line per case is a
        // much cheaper way to be sure of it than re-deriving it every time
        // either side changes. SW_ACT_SEARCH above carries the same guard for
        // the same reason.
        case SW_ACT_FILTER_APPLY:
            if (g.job_running) break;
            g.filter   = g.filter_draft;
            // Cleared before job_start, not after. draw_bottom() now draws the
            // modal only while !st->busy, so a modal left set would no longer
            // be drawn over the busy sheet the way it once was - but it would
            // still be sitting there waiting to reappear the instant the job
            // finished, on top of the results it just fetched. Clearing it here
            // means Apply visibly commits.
            g.ui.modal = SW_MODAL_NONE;
            start_search();
            break;

        case SW_ACT_FILTER_CLEAR:
            if (g.job_running) break;
            // Resets the country/bitrate filter and searches immediately,
            // rather than only resetting the draft and leaving the modal
            // open - simpler, and gives the same immediate feedback Apply
            // does. g.search_q (the free-text part) is deliberately left
            // alone: start_search() re-syncs filter.name from it right after,
            // so "Clear" clears the filter, not what was typed.
            swDirFilterInit(&g.filter);
            g.ui.modal = SW_MODAL_NONE;
            start_search();
            break;

        case SW_ACT_MODAL_CLOSE:
            g.ui.modal = SW_MODAL_NONE;   // discards edits made to filter_draft
            break;

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
    g.ui.now     = g.now;
    g.ui.is_live = (swPlayerState() == SW_PLAY_PLAYING);

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
            // could be, so it wins over whatever the last action said. Copied
            // out under the player's lock rather than read as a raw pointer -
            // this runs every frame, so it is the routine case, not the rare
            // one, for racing the network thread's write to the same bytes.
            {
                char err[96];
                swPlayerError(err, sizeof(err));
                if (err[0]) say(true, "%s", err);
            }
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

// ------------------------------------------------------------- update check
//
// A dedicated thread rather than a JOB_* kind, on purpose: routing this
// through the job worker would either be dropped outright (JOB_TOP already
// owns the worker for the first several seconds after boot) or grey out the
// whole list via g.ui.busy for a check nobody asked for, blocking play on the
// very screen a user opens the app to use.

// Mirrors worker_main's shape - including the __dmb() barrier before clearing
// the running flag, for the same MPCore visibility reason.
//
// Uses g.upd_http rather than plain sw_update_check, so the exit path can
// cancel it - see the struct comment on g.upd_http and swAppRun's exit path.
static void update_check_main(void *arg)
{
    (void)arg;
    g.upd_result = sw_update_check_h(&g.upd_http, g.upd_tag, sizeof(g.upd_tag));
    __dmb();
    g.upd_running = false;
}

// Called once, a little after boot (see swAppRun's frame count) rather than
// immediately, so this never competes with the opening JOB_TOP fetch for
// whatever bandwidth a slow connection has.
static void start_update_check(void)
{
    g.upd_started = true;
    g.upd_running = true;

    // prio + 1: strictly lower priority than the caller, so this can never
    // delay a frame. Deliberately does not touch g.ui.busy - nothing should
    // grey out for a check the user did not ask for.
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g.upd_thr = threadCreate(update_check_main, NULL, 24 * 1024, prio + 1, 1, false);
    if (!g.upd_thr)
        g.upd_thr = threadCreate(update_check_main, NULL, 24 * 1024, prio + 1, 0, false);

    // No message on failure to start it either - silent degradation is the
    // whole point. g.upd_thr staying NULL is what tells the poll in swAppRun
    // there is nothing to join.
    if (!g.upd_thr)
        g.upd_running = false;
}

// Silent by design: SW_UPDATE_CURRENT and every error in sw_update_t produce
// no message and no badge. Only SW_UPDATE_AVAILABLE produces anything, and
// what it produces is passive - a dot on the About tab and a row that changes
// its own text - never a boot-time prompt.
static void finish_update_check(void)
{
    __dmb();
    threadJoin(g.upd_thr, U64_MAX);
    threadFree(g.upd_thr);
    g.upd_thr = NULL;

    if (g.upd_result == SW_UPDATE_AVAILABLE) {
        g.upd_available = true;
        g.ui.tab_badge[SW_TAB_ABOUT] = true;
        // A transient top-screen line. refresh_top_screen() may overwrite
        // this the very next frame with a player error - that is fine, the
        // badge and the About row are the durable signals; this is a bonus.
        say(false, "Version %s is available.", g.upd_tag);
    }
}

// ------------------------------------------------------------------ the loop

void swAppRun(SwPlayerInitResult audio)
{
    memset(&g, 0, sizeof(g));
    g.audio = audio;

    swDirInit();
    swFavLoad();
    swDirFilterInit(&g.filter);
    swDirFilterInit(&g.filter_draft);

    // Said once, up front, rather than only when the user presses A on a
    // station and nothing happens. The app is still fully usable without sound -
    // including the updater, which is how a console in this state gets a build
    // that works.
    if (g.audio != SW_PLAYER_OK)
        say(true, "%s", swPlayerInitTextShort(g.audio));

    // Seed Browse with the built-in presets before the live fetch below even
    // starts, so a user with no network yet - or none for the ~30s this
    // fetch can take - has something to look at instead of an empty list.
    // browse_ready is deliberately left false: JOB_TOP still fires, and on
    // success job_finish() silently replaces this with the real list; on
    // failure it leaves this exactly as it is instead of clearing it.
    int preset_n = swPresetCount();
    if (preset_n > SW_STATIONS_MAX) preset_n = SW_STATIONS_MAX;
    if (preset_n > UI_MAX_ROWS)     preset_n = UI_MAX_ROWS;
    for (int i = 0; i < preset_n; i++)
        swPresetGet(i, &g.browse.items[i]);
    g.browse.count      = preset_n;
    g.browse_is_presets = (preset_n > 0);

    // Open on saved stations when there are any. Someone who has been here
    // before almost always wants one of their own stations, and showing them
    // first means the common case needs no network wait at all.
    g.ui.tab = swFavList()->count > 0 ? SW_TAB_FAVOURITES : SW_TAB_BROWSE;
    if (g.ui.tab == SW_TAB_BROWSE) job_start(JOB_TOP);

    while (swUiRunning()) {
        if (g.ui.busy && !g.job_running) job_finish();

        // ~1.5s in at 60 fps - after the opening frames and the JOB_TOP
        // fetch are already moving, never before.
        g.frame++;
        if (!g.upd_started && g.frame >= 90) start_update_check();
        if (g.upd_started && g.upd_thr && !g.upd_running) finish_update_check();

        refresh_top_screen();
        rebuild_rows();

        SwUiAction a = swUiFrame(&g.ui);
        if (a.kind == SW_ACT_QUIT) break;
        handle(a);
    }

    swPlayerStop();

    if (g.worker) { threadJoin(g.worker, U64_MAX); threadFree(g.worker); g.worker = NULL; }

    // FORMERLY A KNOWN HAZARD: the startup update check had no cancel path at
    // all - sw_update_check ran on a private SwHttp nothing outside it could
    // reach - so a threadJoin here simply waited out whatever the check was
    // still doing if it was still in flight when the app was asked to close:
    // up to CONNECT_TIMEOUT_MS + HANDSHAKE_TIMEOUT_MS + HEAD_TOTAL_MS
    // (8s + 12s + 10s = ~30s, see tcp.c and http.c) if it never got an
    // answer, or however long the server actually took to answer if it did.
    // The comment this replaced also named a hung DNS lookup as unbounded;
    // that turned out not to be the case any more by the time this was
    // fixed - tcp.c's resolve() already bounds a stuck getaddrinfo to
    // RESOLVE_TIMEOUT_MS (5s) independently of any cancel, so that particular
    // fear was stale rather than a second hazard this change also had to
    // close.
    //
    // Closed by giving the check its own caller-owned SwHttp - g.upd_http,
    // used by update_check_main via sw_update_check_h/swHttpGetLocationH
    // instead of the private handle sw_update_check mallocs for itself - and
    // cancelling it right here, before the join below, the same way
    // swPlayerStop cancels g.http and g.ping before joining the player's
    // threads. The cancel reaches whichever stage the check is actually in -
    // resolve, connect, handshake or reading the HEAD response - because
    // swConnCancel sets a flag every one of those wait loops polls, and also
    // calls shutdown() on the socket when one is already open, unsticking a
    // recv() that would otherwise sit until the peer sent something. Safe to
    // call even when no check ever started, or one already finished: g.upd_http
    // is zeroed by swAppRun's top-of-function memset, and swHttpCancel /
    // swConnCancel on a never-opened or already-closed handle only sets that
    // flag - see the comment on SwHttp in http.h and swConnCancel in tcp.c.
    //
    // What this does NOT touch: if the check happens to be stuck inside the
    // resolve's own background thread when this fires, that thread is
    // abandoned rather than killed, per tcp.c's documented resolve() design -
    // this cancel makes the join stop waiting on it, it does not make the
    // getaddrinfo call underneath return any sooner. That is an existing,
    // separately-documented trade in tcp.c (see resolve() and ResolveJob
    // there), not something this change adds or was asked to fix.
    swHttpCancel(&g.upd_http);
    if (g.upd_thr) { threadJoin(g.upd_thr, U64_MAX); threadFree(g.upd_thr); g.upd_thr = NULL; }

    // Arm the restart last, once nothing is still running. The jump itself
    // happens as the process exits.
    if (g.update_installed) sw_update_relaunch();
}
