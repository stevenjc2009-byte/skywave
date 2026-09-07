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
// directory reports the codec, the bitrate and whether a station is HLS, so
// unplayable entries are dropped before anything reaches the screen. HLS is
// excluded by the query itself (hls=0); the codec is checked per station as it
// is parsed, because the app decodes both MP3 and AAC and the API accepts only
// one codec value per query - see codec_playable() in directory_parse.c.
//
// Measured 2026-09-05: the API answers over plain http with no redirect to
// https, which is why this needs no TLS at all.

#include <stdbool.h>
#include <stddef.h>

// Forward-declared, not #included: the full SwHttp definition lives behind
// http.h -> tcp.h, which pulls in mbedtls - fine for directory.c, which needs
// the real thing to call swHttpGetTextBoundedH, but not for something that
// only wants the parsing half of this header (test_directory.c links
// directory_parse.c, never http.c or tcp.c, precisely so it can be proven on
// a PC with no 3DS toolchain). swDirRegisterPlayH only ever passes `h`
// through to http.c, so a pointer to an incomplete type is all it needs here.
typedef struct SwHttp SwHttp;

// Sized from a real response: the longest name across a 30-station sample was
// 56 bytes and the longest resolved URL 280, so these have room without being
// wasteful. One station is 476 bytes.
//
// SW_STATIONS_MAX was 40 up to v1.0.3, and it is a HARD cap: the parse loop in
// directory_parse.c stops copying at it however many stations the server sent.
// Because the codec filter runs AFTER the server has applied its own limit=,
// asking for 40 and then dropping the unplayable ones could only ever show
// fewer than 40 - measured 38 on hardware, and 31 of 40 on the GB sample. The
// query now asks for exactly this many (it is stringified straight into
// QUERY_TAIL), so this is the size of the answer we are willing to hold rather
// than the size of the list a user ends up seeing.
//
// 120 x 476 = 57 KB per list. Three lists live in the app's static state
// (browse, results, job_out), so this costs ~167 KB of BSS against ~56 KB at
// 40 - trivial on a 3DS - and lands 80-90 playable stations after filtering at
// the measured 77.5% retention.
//
// Three other constants are coupled to this one and must move with it, or the
// change either does nothing or breaks the fetch outright:
//   ui.h              UI_MAX_ROWS - a second, independent cap on rows drawn
//   directory.c       JSON_BUF    - a truncated body fails to parse entirely
//   directory_parse.c TOKENS_MAX  - jsmn abandons the parse, it does not truncate
#define SW_STATION_NAME 72
#define SW_STATION_URL  320
#define SW_STATION_UUID 40
#define SW_STATION_CC   40
#define SW_STATIONS_MAX 120

// The largest bitrate worth believing. The directory is community-editable and
// its numeric fields are not validated at the source, so `bitrate` is hostile
// input like everything else in SwStation: json_int() saturates a thirty-digit
// number to INT_MAX rather than overflowing, and without this ceiling that
// saturation reaches the screen as "2147483647 kbps". 2000 is far above any
// real internet radio stream (320 is a high one) and far below anything a
// typo or an attack produces, so a value past it means the bitrate is unknown,
// not enormous. Gate on this before DISPLAYING or reasoning about a bitrate;
// the parser deliberately still stores what it was told.
#define SW_BITRATE_SANE_MAX 2000

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

// ---------------------------------------------------------------- filtering
//
// Everything below exists so the UI can put a filter modal on screen. The short
// version, for anyone wiring that modal up:
//
//   SwDirFilter f;
//   swDirFilterInit(&f);                        // no filters, same as before
//   snprintf(f.name, sizeof(f.name), "%s", typed_text);   // optional
//   f.band = SW_BAND_HIGH;                                // optional
//   snprintf(f.countrycode, sizeof(f.countrycode), "%s",
//            swDirCountries(NULL)[picked].code);          // optional
//   swDirSearch(&f, &list);
//
// The two lists the modal needs - the countries and the bitrate bands - are
// swDirCountries() and swDirBandLabel()/swDirBandRange(). Both are static data;
// neither allocates and neither touches the network.
//
// Every filter here is applied by the server, not by this code. Measured
// 2026-09-06 against de1. and all.: 80 stations returned across two filtered
// queries, zero of them outside the requested country or bitrate band. Pushing
// the filter into the query is also the cheaper half - a filtered answer is a
// smaller answer, and the console has to hold all of it in RAM. Nothing is
// filtered again after parsing.
//
// The MP3-only constraints in the comment at the top of this file are NOT
// optional and are not exposed here: codec=MP3, hls=0 and hidebroken are added
// to every query swDirBuildQuery produces, whatever the filter says.

// ISO 3166-1 alpha-2 country code: two letters plus a terminator.
#define SW_DIR_CC_LEN 3

// Cap on each free-text filter field, terminator included. The longest country
// name the directory uses is 55 bytes ("The United Kingdom Of Great Britain And
// Northern Ireland"), so this has room for the worst real case.
#define SW_DIR_TEXT_MAX 80

// Big enough for the path, every filter percent-encoded at its worst 3x
// expansion, and the fixed tail. swDirBuildQuery refuses rather than truncating,
// so this being too small is a visible failure, not a silently wrong request.
#define SW_DIR_QUERY_MAX 1280

// One row of the country picker.
//
// `code` is what goes in the query; `name` is what goes on screen. The first
// entry of swDirCountries() has an empty `code` and reads "Any country" - it is
// a real row the modal should draw, and selecting it clears the country filter.
typedef struct {
    const char *code;   // ISO 3166-1 alpha-2, e.g. "GB". "" on the "any" row.
    const char *name;   // Short ASCII display name, e.g. "United Kingdom".
} SwDirCountry;

// Bitrate is offered as a handful of bands rather than a number, because a user
// picking from a list cannot ask for something no station has.
//
// The bands do not overlap and, apart from SW_BAND_ANY, all of them exclude
// stations whose bitrate the directory does not know. That is deliberate but
// worth knowing: 6 of 40 stations in a real measured response report bitrate 0,
// and a query with any bitrateMin at all drops them (measured 2026-09-06:
// countrycode=DE returned 6 zero-bitrate stations of 40, the same query with
// bitrateMin=1 returned none).
typedef enum {
    SW_BAND_ANY = 0,    // no bitrate constraint; includes unknown bitrates
    SW_BAND_LOW,        //   1 - 64  kbps
    SW_BAND_MEDIUM,     //  65 - 128 kbps
    SW_BAND_HIGH,       // 129 - 192 kbps
    SW_BAND_TOP,        // over 192  kbps
    SW_BAND_COUNT       // not a band - the number of them, for looping
} SwBitrateBand;

// What the user chose in the filter modal. Zero it with swDirFilterInit and
// then set only the fields being filtered on; an empty string or SW_BAND_ANY
// means "do not filter on this".
typedef struct {
    char name[SW_DIR_TEXT_MAX];         // free text matched against station names
    char tag[SW_DIR_TEXT_MAX];          // e.g. "classical", "news"
    char country[SW_DIR_TEXT_MAX];      // free-text English country name
    char countrycode[SW_DIR_CC_LEN];    // ISO 3166-1 alpha-2, e.g. "ES"
    SwBitrateBand band;
} SwDirFilter;

// Prefer `countrycode` over `country`: the code is an exact match and two bytes
// long, whereas `country` is a substring match against names that run to 55
// characters. `country` exists because swDirByCountry takes one; a filter modal
// should be filling in `countrycode` from swDirCountries().

// Clears every filter. Always call this before setting fields - a filter struct
// left uninitialised will send whatever was on the stack into the query.
void swDirFilterInit(SwDirFilter *f);

// The country picker's rows. `count` may be NULL if the caller does not need
// it. The returned array is static and outlives everything; never free it.
const SwDirCountry *swDirCountries(int *count);

// Index of `code` in swDirCountries(), or -1 if it is not one of them. Matches
// case-insensitively. "" finds the "Any country" row at index 0. For restoring
// the picker's highlight from a saved filter.
int swDirCountryIndex(const char *code);

// On-screen text for a band, e.g. "129 - 192 kbps". Returns "" for a band
// outside SW_BAND_ANY..SW_BAND_TOP rather than NULL, so it is safe to print.
const char *swDirBandLabel(SwBitrateBand b);

// The band's bounds in kbps. Either may be 0, meaning unbounded on that side.
// Both pointers may be NULL. Only needed if the modal wants to show the numbers
// itself instead of the label.
void swDirBandRange(SwBitrateBand b, int *min_kbps, int *max_kbps);

// Runs a filtered search. This is the one the filter modal calls.
//
// Returns SW_DIR_ERR_PARSE without touching the network if the filter is
// malformed - a countrycode that is not exactly two letters, a band outside the
// enum, an unterminated text field, or filters so long the query will not fit.
// None of those are reachable from a modal that picks from swDirCountries() and
// the SwBitrateBand values.
SwDirResult swDirSearch(const SwDirFilter *f, SwStationList *out);

// Builds the path-and-query `swDirSearch` would fetch, into `out`.
//
// Returns false and leaves `out` empty rather than writing a truncated query,
// for the same reason swUrlSplit refuses: a clipped query is not a narrower
// search, it is a different one. Exposed so query construction can be proven on
// the host with no network - see tests/test_directory.c.
bool swDirBuildQuery(const SwDirFilter *f, char *out, size_t cap);

// ------------------------------------------------------------------ queries

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
//
// Fire-and-forget: the SwHttp behind this call is private to it, so nothing
// outside this call can reach it, and nothing outside this call can cancel
// it either. Fine for a caller with nothing else going on; not fine for the
// player's network thread, which is not that caller - see swDirRegisterPlayH.
void swDirRegisterPlay(const char *uuid);

// Same as swDirRegisterPlay, but fetches into `h` instead of a private
// handle, so the caller can hold a pointer to `h` and call swHttpCancel(h)
// from another thread to unblock this ping - the same mechanism swPlayerStop
// already uses on the stream handle itself.
//
// `h` must not be a stack local: sizeof(SwHttp) is 17,584 bytes, and the
// intended caller is the player's network thread, which has a 16 KB stack.
// Give it a static or a zeroed heap block - see swHttpGetTextBoundedH in
// http.h for exactly what "zeroed" buys and why it only matters on the first
// call through a given `h`.
void swDirRegisterPlayH(SwHttp *h, const char *uuid);

// Human-readable text for a failed query, for putting on screen.
const char *swDirErrorText(SwDirResult r);

// Percent-encodes `in` into `out` for use in a query string. Exposed for tests.
void swDirUrlEncode(const char *in, char *out, size_t cap);

// Parses a radio-browser JSON array into `out`. Exposed so the parser can be
// tested on the host against a saved response with no network involved.
SwDirResult swDirParseJson(const char *json, size_t len, SwStationList *out);
