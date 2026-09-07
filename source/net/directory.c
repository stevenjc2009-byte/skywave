// The networked half of the directory client. The parsing lives next door in
// directory_parse.c so it can be tested without a console.

#include "directory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"

// Plain http, deliberately. Measured 2026-09-05: the mirrors serve http without
// redirecting to https, so the whole directory works with no TLS handshake -
// which on a 268 MHz ARM11 is worth having.
//
// `all.` is the round-robin name the maintainers ask clients to use rather than
// pinning a mirror. de1 is the fallback because at the time of writing it is
// the only mirror the SRV record actually advertises; the historical fi1/at1/nl1
// names no longer resolve at all, so hardcoding a list of them would be worse
// than useless.
static const char *kHosts[] = {
    "http://all.api.radio-browser.info",
    "http://de1.api.radio-browser.info",
};
#define HOST_COUNT ((int)(sizeof(kHosts) / sizeof(kHosts[0])))

// A 40-station answer measured about 48 KB. This is roughly double that, so a
// directory that grows a few more fields per station does not start silently
// truncating - which would surface as SW_DIR_ERR_PARSE, not as missing rows.
#define JSON_BUF 98304

// The common query tail - hls=0, hidebroken, the popularity ordering and
// limit=40 - used to be spelled out here. It now lives in directory_parse.c
// next to swDirBuildQuery, which appends it to every query it produces, so that
// no caller can forget it and so that query construction can be proven on the
// host. See QUERY_TAIL there.
//
// The tail no longer carries a codec constraint. It used to read `codec=MP3`,
// which the API applies server-side - but the API takes exactly one codec
// value, so there was no way to ask it for "MP3 or AAC", and once the AAC
// decoder landed that one word was the thing keeping every AAC station off the
// screen. The rule was not dropped, only moved to where it can express both:
// codec_playable() in directory_parse.c, which filters client-side and carries
// the measurements. Note also that `hls=0` is in the tail and is NOT reliable -
// the directory's own hls flag misses stations that serve an HLS manifest, so
// swHttpIsHlsManifest() sniffs the response body as well.

// Timeouts for the register-play ping (swDirRegisterPlay / swDirRegisterPlayH)
// - see the comment at those calls. It answers with one line of JSON that
// nothing here even reads, so it does not need the budget a real query gets: a
// couple of seconds is already generous. These bound only the response head
// and a quiet body, never the connect or handshake before them - real
// cancellation of those is swHttpCancel's job via swDirRegisterPlayH's handle,
// not this constant's.
#define PING_HEAD_TOTAL_MS 2000
#define PING_BODY_IDLE_MS  2000

// Which mirror worked last; tried first next time.
//
// Genuinely touched by more than one thread and deliberately left unguarded.
// The app's worker thread writes it (via the query loop below, on a parse
// success), and the player's network thread reads it (via register_play_url,
// for the register-play ping) - and those are not always the same core, since
// the worker falls back to core 0 when core 1 is unavailable while the player's
// network thread is core 1 only.
//
// Unguarded is still correct here, on three counts, and it is written down
// because "shared mutable static" is exactly the thing a reader should stop at:
//   * It is one naturally-aligned int, so an ARM11 load cannot tear. A reader
//     sees either the old index or the new one, never a mixture.
//   * Every value ever stored is an `h` from the loop below, which is
//     `% HOST_COUNT`, so even a stale read indexes kHosts safely. There is no
//     value of this variable that is not a valid mirror.
//   * The only cost of reading a stale one is that the ping goes to the mirror
//     that worked before this instant instead of the one that worked at it -
//     and the ping is courtesy to the directory whose every failure mode is
//     already ignored on purpose (see swDirRegisterPlayH).
// A lock would buy nothing any of those three do not already give.
static int host_pref = 0;

void swDirInit(void)
{
    host_pref = 0;
}

// Ranks a failure by how much it tells the user, so that when every mirror
// is tried and none succeeds, the result reported is the most informative
// one seen rather than whatever the last mirror happened to produce. Reaching
// a mirror at all - even to find its answer unreadable or empty - says more
// than never getting a response, and knowing the query was understood (even
// with nothing to show for it) says more than a body that would not parse.
static int result_rank(SwDirResult r)
{
    switch (r) {
        case SW_DIR_EMPTY:     return 2;   // reached it, understood it
        case SW_DIR_ERR_PARSE: return 1;   // reached it, could not read it
        case SW_DIR_ERR_NET:
        default:                return 0;   // never reached it
    }
}

// Runs one query path+query against the mirrors until one answers.
static SwDirResult fetch(const char *path_and_query, SwStationList *out)
{
    char *buf = malloc(JSON_BUF);
    if (!buf) return SW_DIR_ERR_NET;

    SwDirResult result = SW_DIR_ERR_NET;

    for (int attempt = 0; attempt < HOST_COUNT; attempt++) {
        int h = (host_pref + attempt) % HOST_COUNT;

        char url[SW_URL_MAX];
        snprintf(url, sizeof(url), "%s%s", kHosts[h], path_and_query);

        int n = swHttpGetText(url, buf, JSON_BUF);
        if (n <= 0) continue;

        // A mirror that goes quiet mid-body for BODY_IDLE_MS still hands back
        // the partial bytes it did get as a positive count (see http.c), so a
        // truncated body reads as "answered" right here even though it is
        // about to fail to parse. Only a real parse success earns the break -
        // a parse failure must fall through and give the other mirror a turn
        // instead of being mistaken for the final answer.
        //
        // host_pref is likewise only updated when the body parsed. It is
        // remembered at all so that a query does not pay the latency of
        // trying a dead mirror first every single time - but remembering a
        // mirror that only half-answered would make every future query start
        // with the flaky one and its truncated bodies. A mirror that parsed to
        // an EMPTY list still counts as good here: it delivered a whole,
        // well-formed body and the emptiness is the answer, not a defect in
        // the delivery.
        // SW_DIR_EMPTY breaks too, and that is not an optimisation - retrying
        // on it is wrong. Every host in kHosts is a mirror of the SAME
        // database, so a body that parsed cleanly to zero stations will parse
        // to zero stations everywhere; the second attempt cannot return a
        // different answer, it can only spend another round trip and another
        // 98,304-byte parse to reach the same one. Only SW_DIR_ERR_PARSE earns
        // the next mirror, because that is the truncation case above - a body
        // this mirror mangled, which another may well deliver whole.
        //
        // This got much easier to hit when codec=MP3 came out of QUERY_TAIL.
        // Before, the server guaranteed MP3-only rows, so a non-empty response
        // always parsed to a non-empty list and only a genuinely empty result
        // set could reach here. Now codec_playable() can empty a full response
        // on its own - a narrow filter over an OGG-heavy country does it - so
        // the pointless retry went from rare to ordinary.
        //
        // MEASURED, before and after, by counting swHttpGetText calls with the
        // network stubbed out (scratchpad/probe_mirror_retry.c): a well-formed
        // all-unplayable response used to try 2 of 2 mirrors and now tries 1,
        // while the control - a response with one playable station - stayed at
        // 1 throughout.
        SwDirResult r = swDirParseJson(buf, (size_t)n, out);
        if (r == SW_DIR_OK || r == SW_DIR_EMPTY) {
            host_pref = h;
            result = r;
            break;
        }

        if (result_rank(r) > result_rank(result)) result = r;
    }

    free(buf);
    return result;
}

// Every query in this file goes through here, so there is one place where a
// filter turns into a URL and one place that can get the encoding wrong. The
// building itself is swDirBuildQuery's job, over in directory_parse.c where it
// can be tested without a console.
static SwDirResult search(const SwDirFilter *f, SwStationList *out)
{
    char q[SW_DIR_QUERY_MAX];
    if (!swDirBuildQuery(f, q, sizeof(q))) return SW_DIR_ERR_PARSE;
    return fetch(q, out);
}

// Copies a caller's string into a filter field. Clipping here is safe in a way
// that clipping the query is not: this is the search term the user typed, and a
// term longer than SW_DIR_TEXT_MAX-1 is already longer than any station name or
// country name the directory holds.
static void set_field(char *dst, const char *src)
{
    snprintf(dst, SW_DIR_TEXT_MAX, "%s", src ? src : "");
}

SwDirResult swDirSearch(const SwDirFilter *f, SwStationList *out)
{
    if (!f || !out) return SW_DIR_ERR_PARSE;
    return search(f, out);
}

SwDirResult swDirTopStations(SwStationList *out)
{
    SwDirFilter f;
    swDirFilterInit(&f);
    return search(&f, out);
}

SwDirResult swDirSearchByName(const char *name, SwStationList *out)
{
    SwDirFilter f;
    swDirFilterInit(&f);
    set_field(f.name, name);
    return search(&f, out);
}

SwDirResult swDirByCountry(const char *country, SwStationList *out)
{
    SwDirFilter f;
    swDirFilterInit(&f);
    set_field(f.country, country);
    return search(&f, out);
}

SwDirResult swDirByTag(const char *tag, SwStationList *out)
{
    SwDirFilter f;
    swDirFilterInit(&f);
    set_field(f.tag, tag);
    return search(&f, out);
}

// Shared by swDirRegisterPlay and swDirRegisterPlayH: builds the one URL both
// need. Kept in one place so there is only one spot that can get the path
// wrong, the same reason `search` exists for the query-building calls above.
static void register_play_url(char *url, size_t cap, const char *uuid)
{
    snprintf(url, cap, "%s/json/url/%s", kHosts[host_pref], uuid);
}

void swDirRegisterPlayH(SwHttp *h, const char *uuid)
{
    if (!h || !uuid || !uuid[0]) return;

    char url[SW_URL_MAX];
    register_play_url(url, sizeof(url), uuid);

    // A small throwaway buffer: the answer is a one-line JSON acknowledgement
    // and nothing here reads it. This is courtesy to the directory, not a
    // dependency of playback, so every failure mode is simply ignored.
    //
    // This runs synchronously on the player's network thread (see
    // player.c's net_main), which is why `h` is the caller's own handle
    // rather than a private one: swPlayerStop needs to be able to call
    // swHttpCancel(h) on this ping the same way it already does on the
    // stream, or Stop / quit can end up blocked on threadJoin for as long as
    // a mirror cares to stall. It cannot be allowed to sit at
    // swHttpGetTextBounded's normal budget either, on top of that - that is
    // sized for pulling a real station list across, not a one-line reply
    // nothing here reads - so it gets the short, bounded timeouts instead.
    char scratch[256];
    (void)swHttpGetTextBoundedH(h, url, scratch, sizeof(scratch),
                                PING_HEAD_TOTAL_MS, PING_BODY_IDLE_MS);
}

void swDirRegisterPlay(const char *uuid)
{
    if (!uuid || !uuid[0]) return;

    // On the heap, same as swHttpGetText's own private handle: SwHttp is
    // around 17 KB. Nothing outside this call ever sees the pointer, so - see
    // the comment on this function in directory.h - nothing outside this call
    // can cancel it either. That is only safe for a caller with nothing else
    // to protect against a hang, which is why player.c uses
    // swDirRegisterPlayH instead of this.
    SwHttp *h = (SwHttp *)malloc(sizeof(SwHttp));
    if (!h) return;
    memset(h, 0, sizeof(*h));

    swDirRegisterPlayH(h, uuid);

    free(h);
}
