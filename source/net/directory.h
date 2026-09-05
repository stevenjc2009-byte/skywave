#pragma once

// The station directory: a thin client for radio-browser.info.
//
// Why a directory at all, rather than a built-in list of stations? Because
// hardcoded stream URLs rot. The ones this project started from are
// undocumented endpoints that the broadcaster can withdraw without warning, and
// an app whose station list is a header file goes stale the first time one
// does. radio-browser is a community-maintained index that already tracks which
// streams are alive.
//
// It is also the reason the app never shows a station it cannot play: the
// directory reports the codec, the bitrate and whether a station is HLS, so the
// query itself filters down to plain MP3. The user never meets a station that
// fails - see swDirQuery.
//
// Measured 2026-09-05: the API answers over plain http with no redirect to
// https, which is why this needs no TLS at all.

#include <stdbool.h>
#include <stddef.h>

// Sized from a real response: the longest name across a 30-station sample was
// 56 bytes and the longest resolved URL 280, so these have room without being
// wasteful. 40 stations at ~470 bytes each is under 19 KB.
#define SW_STATION_NAME 72
#define SW_STATION_URL  320
#define SW_STATION_UUID 40
#define SW_STATION_CC   40
#define SW_STATIONS_MAX 40

typedef struct {
    char name[SW_STATION_NAME];
    char url[SW_STATION_URL];       // url_resolved: redirects already followed
    char uuid[SW_STATION_UUID];
    char country[SW_STATION_CC];
    int  bitrate;                   // kbps, as the directory reports it
} SwStation;

typedef struct {
    SwStation items[SW_STATIONS_MAX];
    int       count;
} SwStationList;

typedef enum {
    SW_DIR_OK = 0,
    SW_DIR_ERR_NET,     // could not reach any mirror
    SW_DIR_ERR_PARSE,   // reached it, could not make sense of the answer
    SW_DIR_EMPTY        // reached it, understood it, nothing matched
} SwDirResult;

// Must be called once before any query. Cheap; does no network I/O.
void swDirInit(void);

// The most-listened-to stations. This is the opening screen, because a radio
// app whose first screen is an empty search box is a radio app nobody uses.
SwDirResult swDirTopStations(SwStationList *out);

// Free-text search over station names.
SwDirResult swDirSearchByName(const char *name, SwStationList *out);

// All stations from one country, most popular first. `country` is the English
// country name the directory uses, e.g. "The United Kingdom".
SwDirResult swDirByCountry(const char *country, SwStationList *out);

// Stations carrying a tag, e.g. "classical", "news", "jazz".
SwDirResult swDirByTag(const char *tag, SwStationList *out);

// Tells the directory a station was played. This is the documented etiquette
// for using the API and it is what keeps the popularity ranking meaningful for
// everyone else. Failure is ignored on purpose - it must never stop playback.
void swDirRegisterPlay(const char *uuid);

// Human-readable text for a failed query, for putting on screen.
const char *swDirErrorText(SwDirResult r);

// Percent-encodes `in` into `out` for use in a query string. Exposed for tests.
void swDirUrlEncode(const char *in, char *out, size_t cap);

// Parses a radio-browser JSON array into `out`. Exposed so the parser can be
// tested on the host against a saved response with no network involved.
SwDirResult swDirParseJson(const char *json, size_t len, SwStationList *out);
