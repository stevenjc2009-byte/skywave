// Host tests for the station-directory JSON parser and URL encoder.
//
// The synthetic fixtures below are hand-written to carry the things that
// actually break a parser: escaped quotes and backslashes in names, \u escapes,
// a station missing its url_resolved, a station missing a name entirely, fields
// in a different order, and a nested array field (`geo_distance` style) that
// the token walker has to step over rather than trip on.
//
// Pass a saved real response as argv[1] to additionally parse genuine API
// output:
//   curl "http://de1.api.radio-browser.info/json/stations/search?codec=MP3&hls=0&limit=30&hidebroken=true" -o real.json
//   ./test_directory real.json

#include "../source/net/directory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

// ------------------------------------------------------------------ encoding

static void test_url_encode(void)
{
    printf("swDirUrlEncode\n");
    char out[256];

    swDirUrlEncode("jazz", out, sizeof(out));
    CHECK(strcmp(out, "jazz") == 0, "plain word changed: '%s'", out);

    // A space must become %20, never '+': this goes into a query the API reads
    // as a literal, and '+' would be searched for as a plus sign.
    swDirUrlEncode("classic fm", out, sizeof(out));
    CHECK(strcmp(out, "classic%20fm") == 0, "got '%s'", out);

    swDirUrlEncode("rock&roll", out, sizeof(out));
    CHECK(strcmp(out, "rock%26roll") == 0, "ampersand would split the query: '%s'", out);

    swDirUrlEncode("a-b_c.d~e", out, sizeof(out));
    CHECK(strcmp(out, "a-b_c.d~e") == 0, "unreserved chars must pass through: '%s'", out);

    // UTF-8 must be encoded byte by byte.
    swDirUrlEncode("caf\xC3\xA9", out, sizeof(out));
    CHECK(strcmp(out, "caf%C3%A9") == 0, "got '%s'", out);

    // Truncation must not run past the buffer or emit half an escape.
    char small[6];
    swDirUrlEncode("&&&&&&&&", small, sizeof(small));
    CHECK(strlen(small) < sizeof(small), "overran: len=%zu", strlen(small));
    CHECK(strcmp(small, "%26") == 0, "expected a clean truncation, got '%s'", small);
}

// ------------------------------------------------------------------- parsing

static const char *kGood =
"["
"{\"changeuuid\":\"x\",\"stationuuid\":\"uuid-one\",\"name\":\"Classic FM\","
 "\"url\":\"http://old/\",\"url_resolved\":\"http://media-ice.musicradio.com/ClassicFMMP3\","
 "\"homepage\":\"http://h/\",\"favicon\":\"\",\"tags\":\"classical\","
 "\"country\":\"The United Kingdom\",\"countrycode\":\"GB\",\"language\":\"english\","
 "\"votes\":10,\"codec\":\"MP3\",\"bitrate\":128,\"hls\":0,\"lastcheckok\":1,"
 "\"geo_lat\":null,\"geo_long\":null,\"has_extended_info\":false},"

// Fields in a different order, an escaped quote and a backslash in the name,
// and no url_resolved - the plain `url` has to be used instead.
"{\"bitrate\":320,\"name\":\"The \\\"Big\\\" Band \\\\ More\",\"stationuuid\":\"uuid-two\","
 "\"country\":\"Germany\",\"url\":\"http://example.org/stream\",\"codec\":\"MP3\"},"

// A \u escape, and a nested array the walker must step over.
"{\"name\":\"Caf\\u00e9 Jazz\",\"stationuuid\":\"uuid-three\","
 "\"extra\":[1,2,{\"deep\":\"value\"}],\"url_resolved\":\"http://jazz/\","
 "\"country\":\"France\",\"bitrate\":96},"

// No name: must be dropped, not shown as a blank row.
"{\"stationuuid\":\"uuid-four\",\"url_resolved\":\"http://nameless/\",\"bitrate\":64},"

// No URL at all: must be dropped, because it cannot be played.
"{\"stationuuid\":\"uuid-five\",\"name\":\"No Stream Here\",\"bitrate\":64}"
"]";

static void test_parse_good(void)
{
    printf("swDirParseJson on a well-formed response\n");

    SwStationList l;
    SwDirResult r = swDirParseJson(kGood, strlen(kGood), &l);

    CHECK(r == SW_DIR_OK, "result was %d", (int)r);
    CHECK(l.count == 3, "expected 3 usable stations of 5, got %d", l.count);
    if (l.count < 3) return;

    CHECK(strcmp(l.items[0].name, "Classic FM") == 0, "name[0]='%s'", l.items[0].name);
    CHECK(strcmp(l.items[0].url, "http://media-ice.musicradio.com/ClassicFMMP3") == 0,
          "url_resolved should win over url: '%s'", l.items[0].url);
    CHECK(strcmp(l.items[0].uuid, "uuid-one") == 0, "uuid[0]='%s'", l.items[0].uuid);
    CHECK(strcmp(l.items[0].country, "The United Kingdom") == 0, "country[0]='%s'", l.items[0].country);
    CHECK(l.items[0].bitrate == 128, "bitrate[0]=%d", l.items[0].bitrate);

    CHECK(strcmp(l.items[1].name, "The \"Big\" Band \\ More") == 0,
          "escapes not decoded: '%s'", l.items[1].name);
    CHECK(strcmp(l.items[1].url, "http://example.org/stream") == 0,
          "should fall back to url: '%s'", l.items[1].url);
    CHECK(l.items[1].bitrate == 320, "bitrate[1]=%d", l.items[1].bitrate);

    CHECK(strcmp(l.items[2].name, "Caf\xC3\xA9 Jazz") == 0,
          "\\u escape not decoded to UTF-8: '%s'", l.items[2].name);
    CHECK(strcmp(l.items[2].url, "http://jazz/") == 0,
          "nested array threw off the walker: '%s'", l.items[2].url);
    CHECK(l.items[2].bitrate == 96, "bitrate[2]=%d", l.items[2].bitrate);
}

static void test_parse_rejects_junk(void)
{
    printf("malformed input is rejected, not half-accepted\n");
    SwStationList l;

    CHECK(swDirParseJson("", 0, &l) == SW_DIR_ERR_PARSE, "empty input");
    CHECK(swDirParseJson("not json", 8, &l) == SW_DIR_ERR_PARSE, "garbage");
    CHECK(swDirParseJson("{\"a\":1}", 7, &l) == SW_DIR_ERR_PARSE,
          "an object is not the array this endpoint returns");
    CHECK(swDirParseJson("[]", 2, &l) == SW_DIR_EMPTY, "an empty array is EMPTY, not an error");
    CHECK(l.count == 0, "count should be 0, was %d", l.count);

    // A truncated response is the realistic failure: the read buffer filled
    // before the array closed. Half a station is worse than none, because its
    // URL would be cut mid-string and would fail at play time with no clue why.
    char cut[512];
    snprintf(cut, sizeof(cut), "%.*s", 200, kGood);
    CHECK(swDirParseJson(cut, strlen(cut), &l) == SW_DIR_ERR_PARSE,
          "a truncated array must not yield partial stations");
}

static void test_overlong_fields_are_truncated_safely(void)
{
    printf("absurdly long fields are truncated, not overflowed\n");

    // A name and URL far longer than the struct can hold. The parser must clip
    // them and keep going rather than write past the field.
    char *js = malloc(4096);
    char longname[600], longurl[900];
    memset(longname, 'N', sizeof(longname) - 1); longname[sizeof(longname) - 1] = 0;
    memset(longurl, 'U', sizeof(longurl) - 1);   longurl[sizeof(longurl) - 1] = 0;
    snprintf(js, 4096,
             "[{\"name\":\"%s\",\"url_resolved\":\"http://%s\",\"bitrate\":128}]",
             longname, longurl);

    SwStationList l;
    SwDirResult r = swDirParseJson(js, strlen(js), &l);

    CHECK(r == SW_DIR_OK, "should still parse, got %d", (int)r);
    CHECK(l.count == 1, "count=%d", l.count);
    if (l.count == 1) {
        CHECK(strlen(l.items[0].name) == SW_STATION_NAME - 1,
              "name should fill the field exactly, len=%zu", strlen(l.items[0].name));
        CHECK(strlen(l.items[0].url) == SW_STATION_URL - 1,
              "url should fill the field exactly, len=%zu", strlen(l.items[0].url));
    }
    free(js);
}

static void test_caps_at_station_max(void)
{
    printf("a response longer than the list cap stops at the cap\n");

    size_t cap = 64 * 1024;
    char *js = malloc(cap);
    size_t o = 0;
    o += (size_t)snprintf(js + o, cap - o, "[");
    for (int i = 0; i < SW_STATIONS_MAX + 20; i++) {
        o += (size_t)snprintf(js + o, cap - o,
                              "%s{\"name\":\"S%d\",\"url_resolved\":\"http://s/%d\",\"bitrate\":128}",
                              i ? "," : "", i, i);
    }
    o += (size_t)snprintf(js + o, cap - o, "]");

    SwStationList l;
    CHECK(swDirParseJson(js, o, &l) == SW_DIR_OK, "should parse");
    CHECK(l.count == SW_STATIONS_MAX, "expected the cap %d, got %d", SW_STATIONS_MAX, l.count);
    free(js);
}

// ------------------------------------------------------------- real response

static void test_real_response(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  (no saved response at %s - skipping)\n", path); return; }

    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *js = malloc((size_t)sz + 1);
    if (!js || fread(js, 1, (size_t)sz, f) != (size_t)sz) {
        printf("  FAIL could not read %s\n", path); failures++; checks++;
        free(js); fclose(f); return;
    }
    js[sz] = 0;
    fclose(f);

    SwStationList l;
    SwDirResult r = swDirParseJson(js, (size_t)sz, &l);

    printf("  %ld bytes of real API output -> %d stations\n", sz, l.count);
    CHECK(r == SW_DIR_OK, "real response did not parse: %s", swDirErrorText(r));
    CHECK(l.count > 0, "no stations recovered from a real response");

    // Every station the parser hands back must be playable as-is: a name to
    // show and an http(s) URL to open. Anything else is a row the user can
    // select and get silence from.
    int bad = 0;
    for (int i = 0; i < l.count; i++) {
        if (!l.items[i].name[0]) bad++;
        else if (strncmp(l.items[i].url, "http", 4) != 0) bad++;
    }
    CHECK(bad == 0, "%d of %d parsed stations are not playable", bad, l.count);

    for (int i = 0; i < l.count && i < 3; i++) {
        printf("    %-30s %4d kbps  %s\n",
               l.items[i].name, l.items[i].bitrate, l.items[i].url);
    }

    free(js);
}

int main(int argc, char **argv)
{
    printf("== directory ==\n");
    test_url_encode();
    test_parse_good();
    test_parse_rejects_junk();
    test_overlong_fields_are_truncated_safely();
    test_caps_at_station_max();

    printf("real API response\n");
    test_real_response(argc > 1 ? argv[1] : "real.json");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
