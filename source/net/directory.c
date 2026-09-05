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

// Common query tail. Every one of these matters:
//   codec=MP3    - the app decodes MP3 and nothing else, so never offer
//                  anything else. The user should not be able to pick a
//                  station that cannot play.
//   hls=0        - HLS streams are playlists, not audio. Excluded for the
//                  same reason.
//   hidebroken   - the directory health-checks streams; there is no reason to
//                  show one it already knows is down.
//   order/reverse- most-clicked first, so the first screenful is the stations
//                  people actually listen to.
#define QUERY_TAIL "codec=MP3&hls=0&hidebroken=true&order=clickcount&reverse=true&limit=40"

static int host_pref = 0;   // which mirror worked last; tried first next time

void swDirInit(void)
{
    host_pref = 0;
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

        // Remember what worked. Falling back on every single query when the
        // first mirror is down would double the latency of the whole app.
        host_pref = h;
        result = swDirParseJson(buf, (size_t)n, out);
        break;
    }

    free(buf);
    return result;
}

SwDirResult swDirTopStations(SwStationList *out)
{
    return fetch("/json/stations/search?" QUERY_TAIL, out);
}

SwDirResult swDirSearchByName(const char *name, SwStationList *out)
{
    char enc[256];
    swDirUrlEncode(name, enc, sizeof(enc));

    char q[SW_URL_MAX];
    snprintf(q, sizeof(q), "/json/stations/search?name=%s&" QUERY_TAIL, enc);
    return fetch(q, out);
}

SwDirResult swDirByCountry(const char *country, SwStationList *out)
{
    char enc[256];
    swDirUrlEncode(country, enc, sizeof(enc));

    char q[SW_URL_MAX];
    snprintf(q, sizeof(q), "/json/stations/search?country=%s&" QUERY_TAIL, enc);
    return fetch(q, out);
}

SwDirResult swDirByTag(const char *tag, SwStationList *out)
{
    char enc[256];
    swDirUrlEncode(tag, enc, sizeof(enc));

    char q[SW_URL_MAX];
    snprintf(q, sizeof(q), "/json/stations/search?tag=%s&" QUERY_TAIL, enc);
    return fetch(q, out);
}

void swDirRegisterPlay(const char *uuid)
{
    if (!uuid || !uuid[0]) return;

    char url[SW_URL_MAX];
    snprintf(url, sizeof(url), "%s/json/url/%s", kHosts[host_pref], uuid);

    // A small throwaway buffer: the answer is a one-line JSON acknowledgement
    // and nothing here reads it. This is courtesy to the directory, not a
    // dependency of playback, so every failure mode is simply ignored.
    char scratch[256];
    (void)swHttpGetText(url, scratch, sizeof(scratch));
}
