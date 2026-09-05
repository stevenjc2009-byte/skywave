// The pure half of the directory client: turning radio-browser's JSON into
// SwStation records, and percent-encoding query strings.
//
// Deliberately separate from directory.c and free of any 3DS header, so the
// parsing can be tested on a PC against a saved response. Parsers are where
// this kind of code goes wrong, and a parser that can only be exercised by
// putting a console on wifi is a parser nobody exercises.

#include "directory.h"

#include <stdlib.h>
#include <string.h>

#define JSMN_STATIC
#include "../../deps/jsmn.h"

// A 40-station response measured ~48 KB and about 70 tokens per station.
// 6144 leaves a wide margin for the directory growing new fields.
#define TOKENS_MAX 6144

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

static int json_int(const char *js, const jsmntok_t *t)
{
    int v = 0;
    for (int i = t->start; i < t->end; i++) {
        char c = js[i];
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
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
            }

            j = skip(tok, j + 1);   // step over key and value together
        }

        // A station with no name or no stream URL is not something the user can
        // be shown or play, so it is dropped rather than displayed as blank.
        if (have_name && st.url[0]) out->items[out->count++] = st;

        i = skip(tok, i);
    }

    free(tok);
    return out->count > 0 ? SW_DIR_OK : SW_DIR_EMPTY;
}
