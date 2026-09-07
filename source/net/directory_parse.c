// The pure half of the directory client: turning radio-browser's JSON into
// SwStation records, and percent-encoding query strings.
//
// Deliberately separate from directory.c and free of any 3DS header, so the
// parsing can be tested on a PC against a saved response. Parsers are where
// this kind of code goes wrong, and a parser that can only be exercised by
// putting a console on wifi is a parser nobody exercises.

#include "directory.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSMN_STATIC
#include "../../deps/jsmn.h"

// A 40-station response measured ~48 KB and about 70 tokens per station.
//
// This is not a soft budget: jsmn returns JSMN_ERROR_NOMEM and abandons the
// WHOLE parse the moment it runs out of slots, so an undersized TOKENS_MAX
// turns a big response into SW_DIR_ERR_PARSE - no stations at all, rather than
// the first N. At the measured ~70 tokens per station, the 120 stations this
// now asks for need ~8,400, which the old 6144 would have failed outright.
// 16384 keeps roughly the same 2x margin the original had at 40 stations.
#define TOKENS_MAX 16384

const char *swDirErrorText(SwDirResult r)
{
    switch (r) {
        case SW_DIR_OK:        return "";
        case SW_DIR_ERR_NET:   return "Could not reach the station directory.";
        case SW_DIR_ERR_PARSE: return "The directory sent something unreadable.";
        case SW_DIR_EMPTY:     return "No stations matched.";
    }
    return "Something went wrong.";
}

// ------------------------------------------------------------------ encoding

void swDirUrlEncode(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;

    // `in` is checked because this one is public API - directory.h exposes it
    // so the tests can reach it - and nothing in that header rules a NULL out.
    // The hostile-input suite handed it one and UBSan reported
    //   directory_parse.c:40 runtime error: load of null pointer of type
    //   'const unsigned char'
    // which on the console, with no sanitizer and no memory protection worth
    // the name, is a straight crash. An empty string is left behind rather
    // than nothing, the same way swDirBuildQuery zeroes `out` when it refuses,
    // so a caller that ignores the emptiness still reads a terminated buffer.
    if (!in) {
        if (cap) out[0] = 0;
        return;
    }

    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;

        // RFC 3986 unreserved. Everything else is escaped, including spaces -
        // '+' is only valid in form bodies, not in a path or query the way
        // this API reads them.
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~';

        if (safe) {
            if (o + 1 >= cap) break;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= cap) break;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }

    if (cap) out[o < cap ? o : cap - 1] = 0;
}

// ----------------------------------------------------------------- filtering

// The constraints every query carries no matter what the user filtered on.
// Moved here from directory.c so there is exactly one place that can drop them:
//   hls=0        - HLS is a manifest listing segment files, not a stream. The
//                  app issues one GET and reads bytes until it is told to stop,
//                  so an HLS entry can never play no matter which decoder it
//                  would have needed. This one stays in the query.
//   hidebroken   - the directory health-checks streams; there is no reason to
//                  show one it already knows is down.
//   order/reverse- most-clicked first, so the first screenful is the stations
//                  people actually listen to.
//
// `codec=MP3` USED TO BE HERE and has deliberately been removed. The API takes
// a single codec value with no way to express "MP3 or AAC", so once the AAC
// decoder landed this line was the thing keeping every AAC station off the
// screen - the decoder was finished, linked, and unreachable. The rule it
// enforced has not been dropped, only moved to where it can express both: see
// codec_playable() below, which also carries the measurements.
//
// limit= was 40 and is now 120. The filter below runs on the CLIENT, after the
// server has already truncated to limit=, so every station it drops is a row
// that can never come back - which is why v1.0.3 showed 38 rather than 40. The
// query now asks for SW_STATIONS_MAX so the filter has spare rows to eat: at
// the measured GB retention of 31/40 (77.5%), 120 requested lands ~93 shown.
//
// The number is STRINGIFIED from SW_STATIONS_MAX rather than written out, so
// the two cannot drift apart: asking for more than the parser will store wastes
// bandwidth and tokens, and asking for fewer caps the list below its own array.
// See directory.h for the other constants coupled to this one.
#define SW_STR2(x) #x
#define SW_STR(x)  SW_STR2(x)
#define QUERY_TAIL \
    "hls=0&hidebroken=true&order=clickcount&reverse=true&limit=" SW_STR(SW_STATIONS_MAX)

// Why a built-in list rather than the directory's /json/countries endpoint.
//
// Measured 2026-09-06: that endpoint answers 1811 bytes for the 30 most-stationed
// countries, and names them the long way - "The United Kingdom Of Great Britain
// And Northern Ireland" is 55 characters and will not fit a row on the bottom
// screen, and Turkey arrives as non-ASCII "Turkiye". Using it would cost a
// second request, a second parser (this one reads station arrays, not country
// arrays) and a heap buffer, all before the filter modal could open - and it
// would leave the filter dead exactly when the directory is unreachable, which
// is when a user is most likely to be poking at it. The only part that has to
// be right is the code, and ISO 3166-1 alpha-2 does not change.
//
// The countries are the 30 with the most stations, read off
// http://de1.api.radio-browser.info/json/countries?order=stationcount&reverse=true
// on 2026-09-06. Display names are short and ASCII so they fit and always draw.
// Ordered alphabetically by display name, because a list this long is scanned
// for a known name rather than browsed. Index 0 is the "any" row - see the
// SwDirCountry comment in directory.h.
static const SwDirCountry kCountries[] = {
    { "",   "Any country" },
    { "AR", "Argentina" },
    { "AU", "Australia" },
    { "AT", "Austria" },
    { "BE", "Belgium" },
    { "BR", "Brazil" },
    { "CA", "Canada" },
    { "CL", "Chile" },
    { "CN", "China" },
    { "CO", "Colombia" },
    { "FR", "France" },
    { "DE", "Germany" },
    { "GR", "Greece" },
    { "HU", "Hungary" },
    { "IN", "India" },
    { "ID", "Indonesia" },
    { "IT", "Italy" },
    { "MX", "Mexico" },
    { "NL", "Netherlands" },
    { "PH", "Philippines" },
    { "PL", "Poland" },
    { "RO", "Romania" },
    { "RU", "Russia" },
    { "RS", "Serbia" },
    { "ES", "Spain" },
    { "CH", "Switzerland" },
    { "TR", "Turkey" },
    { "UA", "Ukraine" },
    { "AE", "United Arab Emirates" },
    { "GB", "United Kingdom" },
    { "US", "United States" },
};
#define COUNTRY_COUNT ((int)(sizeof(kCountries) / sizeof(kCountries[0])))

// Bounds chosen around the bitrates stations actually report - a real 40-station
// sample held only 0, 56, 64, 96, 128, 192 and 320 - so each band has something
// in it. A 0 bound means unbounded on that side and is simply not sent.
typedef struct {
    const char *label;
    int         min_kbps;
    int         max_kbps;
} BandDef;

static const BandDef kBands[SW_BAND_COUNT] = {
    { "Any bitrate",    0,   0   },
    { "Up to 64 kbps",  1,   64  },
    { "65 - 128 kbps",  65,  128 },
    { "129 - 192 kbps", 129, 192 },
    { "Over 192 kbps",  193, 0   },
};

void swDirFilterInit(SwDirFilter *f)
{
    if (f) memset(f, 0, sizeof(*f));
}

const SwDirCountry *swDirCountries(int *count)
{
    if (count) *count = COUNTRY_COUNT;
    return kCountries;
}

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool is_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int swDirCountryIndex(const char *code)
{
    if (!code) return -1;

    for (int i = 0; i < COUNTRY_COUNT; i++) {
        const char *k = kCountries[i].code;
        int j = 0;
        while (k[j] && code[j] && up(k[j]) == up(code[j])) j++;
        if (!k[j] && !code[j]) return i;
    }
    return -1;
}

const char *swDirBandLabel(SwBitrateBand b)
{
    // Compared as unsigned deliberately. devkitARM builds with -fshort-enums,
    // so this enum is an unsigned byte on the console and a signed int on the
    // host - a `< 0` test is dead code there and warns. Widening to unsigned
    // catches a negative on the host too, since it wraps to a huge value.
    if ((unsigned)b >= SW_BAND_COUNT) return "";
    return kBands[b].label;
}

void swDirBandRange(SwBitrateBand b, int *min_kbps, int *max_kbps)
{
    bool ok = ((unsigned)b < SW_BAND_COUNT);   // see swDirBandLabel
    if (min_kbps) *min_kbps = ok ? kBands[b].min_kbps : 0;
    if (max_kbps) *max_kbps = ok ? kBands[b].max_kbps : 0;
}

// ------------------------------------------------------- query construction

// Length of a string known to live in a fixed array, refusing to read past it.
// Returns `cap` if no terminator was found, which callers treat as malformed -
// an unterminated filter field would otherwise walk off the struct and into the
// query.
static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

// Appends `s`, refusing rather than clipping. `*o` is only advanced on success,
// so a failed append leaves a shorter but still valid string behind - the caller
// abandons the whole query anyway.
static bool q_put(char *out, size_t cap, size_t *o, const char *s)
{
    size_t n = strlen(s);
    if (*o + n + 1 > cap) return false;
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = 0;
    return true;
}

// Appends `key=<percent-encoded val>&`, or nothing at all if `val` is empty.
//
// The encode buffer is three times SW_DIR_TEXT_MAX because every byte can
// become "%XX" - a country name that is entirely non-ASCII hits exactly that.
// swDirUrlEncode truncates silently when it runs out of room, and a truncated
// country name is a different country, so the length is checked first and the
// buffer sized so the encode can never be the thing that clips.
static bool q_param(char *out, size_t cap, size_t *o, const char *key, const char *val)
{
    size_t n = bounded_len(val, SW_DIR_TEXT_MAX);
    if (n >= SW_DIR_TEXT_MAX) return false;     // unterminated field
    if (n == 0) return true;                    // not filtering on this

    char enc[SW_DIR_TEXT_MAX * 3];
    swDirUrlEncode(val, enc, sizeof(enc));

    return q_put(out, cap, o, key) &&
           q_put(out, cap, o, "=") &&
           q_put(out, cap, o, enc) &&
           q_put(out, cap, o, "&");
}

static bool q_int_param(char *out, size_t cap, size_t *o, const char *key, int v)
{
    if (v <= 0) return true;                    // unbounded on this side

    char buf[32];
    snprintf(buf, sizeof(buf), "%s=%d&", key, v);
    return q_put(out, cap, o, buf);
}

bool swDirBuildQuery(const SwDirFilter *f, char *out, size_t cap)
{
    if (!f || !out || cap == 0) return false;
    out[0] = 0;

    size_t o = 0;
    if (!q_put(out, cap, &o, "/json/stations/search?")) return false;

    if (!q_param(out, cap, &o, "name",    f->name))    goto fail;
    if (!q_param(out, cap, &o, "country", f->country)) goto fail;
    if (!q_param(out, cap, &o, "tag",     f->tag))     goto fail;

    // countrycode is not free text: it comes from kCountries, so anything that
    // is not exactly two letters means the caller built the filter by hand and
    // got it wrong. Refusing is better than sending it - "countrycode=Spain"
    // matches nothing and would read on screen as "no stations in Spain".
    {
        size_t n = bounded_len(f->countrycode, SW_DIR_CC_LEN);
        if (n >= SW_DIR_CC_LEN) goto fail;              // unterminated
        if (n != 0) {
            if (n != 2 || !is_alpha(f->countrycode[0]) || !is_alpha(f->countrycode[1]))
                goto fail;

            char cc[SW_DIR_CC_LEN] = { up(f->countrycode[0]), up(f->countrycode[1]), 0 };
            if (!q_put(out, cap, &o, "countrycode=")) goto fail;
            if (!q_put(out, cap, &o, cc))            goto fail;
            if (!q_put(out, cap, &o, "&"))           goto fail;
        }
    }

    if ((unsigned)f->band >= SW_BAND_COUNT) goto fail;   // see swDirBandLabel
    if (!q_int_param(out, cap, &o, "bitrateMin", kBands[f->band].min_kbps)) goto fail;
    if (!q_int_param(out, cap, &o, "bitrateMax", kBands[f->band].max_kbps)) goto fail;

    if (!q_put(out, cap, &o, QUERY_TAIL)) goto fail;
    return true;

fail:
    out[0] = 0;
    return false;
}

// --------------------------------------------------------------------- jsmn

static bool tok_is_key(const char *js, const jsmntok_t *t, const char *key)
{
    if (t->type != JSMN_STRING) return false;
    size_t len = (size_t)(t->end - t->start);
    return strlen(key) == len && strncmp(js + t->start, key, len) == 0;
}

// Writes one UTF-8 encoding of `cp` and returns how many bytes it took.
static size_t utf8_put(char *dst, size_t room, unsigned cp)
{
    if (cp < 0x80)          { if (room < 1) return 0; dst[0] = (char)cp; return 1; }
    if (cp < 0x800)         { if (room < 2) return 0;
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (room < 3) return 0;
    dst[0] = (char)(0xE0 | (cp >> 12));
    dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static unsigned hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    return v;
}

// Copies a JSON string token into `dst`, undoing escapes. Station names come
// back with real quotes and backslashes in them often enough that copying the
// raw bytes shows users a literal \" on screen.
static void json_copy(const char *js, const jsmntok_t *t, char *dst, size_t cap)
{
    if (cap == 0) return;

    size_t o = 0;
    for (int i = t->start; i < t->end && o + 1 < cap; i++) {
        char c = js[i];

        if (c != '\\' || i + 1 >= t->end) {
            dst[o++] = c;
            continue;
        }

        char e = js[++i];
        switch (e) {
            case 'n': dst[o++] = '\n'; break;
            case 't': dst[o++] = '\t'; break;
            case 'r': dst[o++] = '\r'; break;
            case 'b': dst[o++] = '\b'; break;
            case 'f': dst[o++] = '\f'; break;
            case '"': dst[o++] = '"';  break;
            case '\\': dst[o++] = '\\'; break;
            case '/': dst[o++] = '/';  break;
            case 'u': {
                if (i + 4 < t->end) {
                    unsigned cp = hex4(js + i + 1);
                    i += 4;
                    // Surrogate pairs are not decoded: nothing outside the BMP
                    // appears in a station name, and a lone replacement char
                    // is better than a half-formed sequence.
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                    o += utf8_put(dst + o, cap - 1 - o, cp ? cp : 0xFFFD);
                }
                break;
            }
            default: dst[o++] = e; break;
        }
    }

    dst[o] = 0;
}

// Saturates at INT_MAX instead of wrapping. The digits come from a third-party
// directory mirror, and a "bitrate" of thirty nines made UBSan report
//   directory_parse.c:387 runtime error: signed integer overflow:
//   999999999 * 10 cannot be represented in type 'int'
// which is undefined behaviour. On the console there is no sanitizer, so the
// accumulator just wraps - and a wrap is the dangerous outcome, because it can
// land an absurd number back inside a plausible band and read on screen as a
// perfectly ordinary 128 kbps station.
//
// Bounding the digit count instead would also stop the trap, but it would
// silently shorten a legitimate long value into a smaller wrong one, and this
// helper is not promised only to the bitrate field - it is the numeric reader
// for whatever field the parser grows next. Saturating keeps every in-range
// value exact and leaves an out-of-range one visibly absurd.
static int json_int(const char *js, const jsmntok_t *t)
{
    int v = 0;
    for (int i = t->start; i < t->end; i++) {
        char c = js[i];
        if (c < '0' || c > '9') break;
        int d = c - '0';
        if (v > (INT_MAX - d) / 10) return INT_MAX;
        v = v * 10 + d;
    }
    return v;
}

// Returns the index just past the subtree rooted at `i`.
static int skip(const jsmntok_t *tok, int i)
{
    int n = tok[i].size;
    i++;
    if (tok[i - 1].type == JSMN_OBJECT) {
        for (int k = 0; k < n; k++) {
            i = skip(tok, i);   // the key
            i = skip(tok, i);   // the value
        }
    } else if (tok[i - 1].type == JSMN_ARRAY) {
        for (int k = 0; k < n; k++) i = skip(tok, i);
    }
    return i;
}

// ------------------------------------------------------------------- parsing

// Can this app actually decode what the directory says the station sends?
//
// This moved out of the query and into here when AAC playback landed. It used
// to be `codec=MP3` in QUERY_TAIL, which the server applied - but the API takes
// exactly one codec value, so there was no way to ask it for "MP3 or AAC" and
// the constraint silently kept every AAC station off the screen long after the
// decoder for them was finished and linked in. Measured 2026-09-07 against
// de1.api.radio-browser.info, hls=0&hidebroken=true&order=clickcount&limit=40:
//
//   countrycode=GB : MP3 19, UNKNOWN 9, AAC 7, AAC+ 5
//   name=jazz      : MP3 29, AAC 6, AAC+ 4, OGG 1
//
// So the old query was showing 19 of 40 UK stations and hiding 12 the app can
// now play.
//
// CORRECTED 2026-09-07, v1.0.4. This comment used to end: "Filtering here
// instead costs one OGG station's worth of wasted row in 80 measured - the
// response is 52,800 bytes against a 98,304-byte buffer, so limit=40 did not
// need raising to compensate." That was wrong, and wrong in a way worth
// recording: it measured the BUFFER and concluded about the ROW COUNT. Those
// are different quantities. limit= is applied by the SERVER; this filter runs
// on the CLIENT afterwards, so every row it drops is a row that cannot come
// back, and the list can only ever be shorter than limit=. The GB numbers above
// say so directly - 31 of 40 survive - and the shipped v1.0.3 showed 38.
// The query now asks for SW_STATIONS_MAX (120) so the filter has spare rows to
// consume. See directory.h for the constants this is coupled to.
//
// What is accepted:
//   MP3            - mpg123.
//   AAC, AAC+/AACP - Helix, including HE-AAC: the full SBR set (sbr.c, sbrqmf.c,
//                    sbrhfgen.c and the rest) is compiled in, so the "+" is real
//                    support and not a hopeful match on a prefix.
//
// What is NOT, and why UNKNOWN is on this side of the line. This started as a
// principled argument - never offer a station that cannot play, do not guess on
// the user's behalf - and the argument was worth exactly as much as any
// unmeasured argument. MEASURED 2026-09-07 instead: every codec="UNKNOWN"
// station reachable from four live queries (countrycode=GB, countrycode=ES,
// tag=news, unfiltered) was pulled and its first bytes classified by the same
// syncword test player.c applies. 15 of 15 were HLS or playlist manifests -
// BBC Radio 2, 3, 4, 4 Extra, 5 Live, 6 Music, France Inter, RMC, Cadena 100,
// RNE Radio 3 and the rest. ZERO were MP3 and ZERO were AAC.
//
// So UNKNOWN is not "might work" - on this evidence it is how radio-browser
// spells HLS when its own scanner could not identify the stream. And note what
// that means: those 15 arrived DESPITE hls=0 being in the query, so the
// directory's hls flag misses them and this check is the thing catching them.
// Refusing UNKNOWN is doing HLS filtering, not codec filtering. If a future
// sample turns up an UNKNOWN that really is MP3, revisit this - but revisit it
// with a measurement, not with the argument this paragraph replaced.
//
// The runtime does not rely on any of this being right in any case: the player
// sniffs the real bytes for an ADTS syncword rather than trusting a label.
//
// An ABSENT codec key is kept, not dropped. Measured across 280 stations in 7
// live queries: the field is never empty and never missing, so this branch is
// unreachable from the real API. It exists for hand-built objects - the test
// fixtures are full of them - where dropping on absence would fail them for a
// reason that has nothing to do with codecs.
static bool codec_playable(const char *codec)
{
    if (!codec[0]) return true;         // absent: fixtures only, see above

    static const char *kOk[] = { "MP3", "AAC", "AAC+", "AACP" };
    for (size_t i = 0; i < sizeof(kOk) / sizeof(kOk[0]); i++) {
        const char *a = codec, *b = kOk[i];
        while (*a && *b && up((char)*a) == up((char)*b)) { a++; b++; }
        if (!*a && !*b) return true;
    }
    return false;
}

SwDirResult swDirParseJson(const char *json, size_t len, SwStationList *out)
{
    out->count = 0;

    jsmntok_t *tok = malloc(sizeof(jsmntok_t) * TOKENS_MAX);
    if (!tok) return SW_DIR_ERR_PARSE;

    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, json, len, tok, TOKENS_MAX);

    // A truncated response is the normal failure here: the buffer filled before
    // the array closed. Partial results would be worse than none, because the
    // last station in the list would have a half-copied URL.
    if (n < 1 || tok[0].type != JSMN_ARRAY) {
        free(tok);
        return SW_DIR_ERR_PARSE;
    }

    int stations = tok[0].size;
    int i = 1;

    for (int s = 0; s < stations && out->count < SW_STATIONS_MAX; s++) {
        if (i >= n) break;
        if (tok[i].type != JSMN_OBJECT) { i = skip(tok, i); continue; }

        SwStation st;
        memset(&st, 0, sizeof(st));
        bool have_url = false, have_name = false;

        // Not a member of SwStation: it is read, used to decide, and thrown
        // away. Forty SwStations live in a SwStationList, so a field nothing
        // downstream reads would be forty copies of dead weight in RAM on a
        // console that has little of it. "AAC+" is the longest value that
        // matters; the buffer has room for the longer ones the API sends
        // (UNKNOWN, FLAC, OPUS) so they compare as themselves rather than
        // being truncated into something that might accidentally match.
        char codec[16] = { 0 };

        int fields = tok[i].size;
        int j = i + 1;

        for (int f = 0; f < fields && j + 1 < n; f++) {
            const jsmntok_t *k = &tok[j];
            const jsmntok_t *v = &tok[j + 1];

            if (tok_is_key(json, k, "name")) {
                json_copy(json, v, st.name, sizeof(st.name));
                have_name = st.name[0] != 0;
            } else if (tok_is_key(json, k, "url_resolved")) {
                // url_resolved has already had redirects followed by the
                // directory's own checker, which saves the console doing it.
                json_copy(json, v, st.url, sizeof(st.url));
                if (st.url[0]) have_url = true;
            } else if (tok_is_key(json, k, "url") && !have_url) {
                // Only as a fallback: some entries have no resolved URL yet.
                json_copy(json, v, st.url, sizeof(st.url));
            } else if (tok_is_key(json, k, "stationuuid")) {
                json_copy(json, v, st.uuid, sizeof(st.uuid));
            } else if (tok_is_key(json, k, "country")) {
                json_copy(json, v, st.country, sizeof(st.country));
            } else if (tok_is_key(json, k, "bitrate")) {
                st.bitrate = json_int(json, v);
            } else if (tok_is_key(json, k, "codec")) {
                json_copy(json, v, codec, sizeof(codec));
            }

            j = skip(tok, j + 1);   // step over key and value together
        }

        // A station with no name or no stream URL is not something the user can
        // be shown or play, so it is dropped rather than displayed as blank.
        // A codec this app has no decoder for is dropped for the same reason -
        // see codec_playable() for which ones those are and what it costs.
        if (have_name && st.url[0] && codec_playable(codec))
            out->items[out->count++] = st;

        i = skip(tok, i);
    }

    free(tok);
    return out->count > 0 ? SW_DIR_OK : SW_DIR_EMPTY;
}
