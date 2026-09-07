// Host tests for the station-directory JSON parser, URL encoder and filter
// query builder.
//
// The filter half is testable here at all because swDirBuildQuery is pure and
// lives in directory_parse.c: a query string can be compared byte for byte on a
// PC, whereas the same mistake on hardware shows up as "No stations matched"
// with nothing to look at.
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

// ------------------------------------------------------------------ filters

// The tail swDirBuildQuery appends to every query. Spelled out here rather than
// shared with the implementation on purpose: a test that imported the same
// macro would agree with any change to it, including a mistaken one, and this
// tail is exactly where a mistake goes unnoticed.
//
// codec=MP3 USED TO BE PART OF THIS and was removed on 2026-09-07. It is worth
// knowing why the change had to happen here rather than being absorbed: while
// it was pinned, the server never returned an AAC station, so the AAC decoder
// that had already shipped could not be reached by any query - and no test in
// this suite could have noticed, because they all asserted the constraint was
// present. The rule moved to codec_playable() in directory_parse.c, where it
// can name both codecs; test_parse_codec_filter and test_query_does_not_pin_codec
// are the two halves that now hold it in place.
#define TAIL "hls=0&hidebroken=true&order=clickcount&reverse=true&limit=40"
#define SEARCH "/json/stations/search?"

static void test_country_list(void)
{
    printf("the country list the filter modal shows\n");

    int n = -1;
    const SwDirCountry *c = swDirCountries(&n);

    CHECK(c != NULL, "country list is NULL");
    CHECK(n > 10, "expected a useful number of countries, got %d", n);

    // Row 0 is the "no country filter" row and must be drawable like any other.
    CHECK(strcmp(c[0].code, "") == 0, "row 0 should have an empty code, has '%s'", c[0].code);
    CHECK(c[0].name != NULL && c[0].name[0] != 0, "row 0 needs a label to draw");

    // count may be NULL - a caller that already knows the length.
    CHECK(swDirCountries(NULL) == c, "swDirCountries(NULL) returned a different array");

    int bad_code = 0, bad_name = 0, dup = 0, unsorted = 0;
    for (int i = 1; i < n; i++) {
        if (strlen(c[i].code) != 2 ||
            c[i].code[0] < 'A' || c[i].code[0] > 'Z' ||
            c[i].code[1] < 'A' || c[i].code[1] > 'Z') bad_code++;

        if (!c[i].name || !c[i].name[0]) bad_name++;

        // Non-ASCII would depend on the console font having the glyph; the
        // whole reason these names are hand-written is to avoid that.
        for (const unsigned char *p = (const unsigned char *)c[i].name; *p; p++)
            if (*p > 0x7F) { bad_name++; break; }

        for (int j = 1; j < i; j++) if (strcmp(c[i].code, c[j].code) == 0) dup++;

        if (i > 1 && strcmp(c[i - 1].name, c[i].name) >= 0) unsorted++;
    }
    CHECK(bad_code == 0, "%d codes are not two uppercase ISO letters", bad_code);
    CHECK(bad_name == 0, "%d names are empty or non-ASCII", bad_name);
    CHECK(dup == 0, "%d duplicate country codes", dup);
    CHECK(unsorted == 0, "%d rows out of alphabetical order - the list is scanned, not browsed", unsorted);

    // The ones a user of this app is most likely to reach for.
    const char *want[] = { "GB", "ES", "US", "DE", "FR", "IN" };
    for (size_t w = 0; w < sizeof(want) / sizeof(want[0]); w++)
        CHECK(swDirCountryIndex(want[w]) > 0, "%s missing from the country list", want[w]);
}

static void test_country_index(void)
{
    printf("swDirCountryIndex\n");

    int n = 0;
    const SwDirCountry *c = swDirCountries(&n);

    // Every row must find itself, or the modal cannot restore its highlight
    // from a saved filter.
    int lost = 0;
    for (int i = 0; i < n; i++) if (swDirCountryIndex(c[i].code) != i) lost++;
    CHECK(lost == 0, "%d rows do not round-trip through swDirCountryIndex", lost);

    CHECK(swDirCountryIndex("") == 0, "the empty code is the 'any country' row");
    CHECK(swDirCountryIndex("gb") == swDirCountryIndex("GB"), "matching must be case-insensitive");
    CHECK(swDirCountryIndex("ZZ") == -1, "an unknown code must not match");
    CHECK(swDirCountryIndex("G") == -1, "a prefix must not match a two-letter code");
    CHECK(swDirCountryIndex("GBR") == -1, "alpha-3 is not what this list holds");
    CHECK(swDirCountryIndex(NULL) == -1, "NULL must not crash or match");
}

static void test_bitrate_bands(void)
{
    printf("bitrate bands\n");

    CHECK(SW_BAND_ANY == 0, "SW_BAND_ANY must be the zero value, so a memset filter is unfiltered");
    CHECK((int)SW_BAND_COUNT == 5, "expected 5 bands, got %d", (int)SW_BAND_COUNT);

    int lo = -1, hi = -1;
    swDirBandRange(SW_BAND_ANY, &lo, &hi);
    CHECK(lo == 0 && hi == 0, "SW_BAND_ANY must be unbounded both ways, got %d..%d", lo, hi);

    int prev_max = 0;
    for (int b = SW_BAND_LOW; b < SW_BAND_COUNT; b++) {
        const char *label = swDirBandLabel((SwBitrateBand)b);
        CHECK(label && label[0], "band %d has no label to draw", b);

        swDirBandRange((SwBitrateBand)b, &lo, &hi);

        // Every band but ANY sets a minimum, which is also what excludes the
        // stations the directory reports as bitrate 0.
        CHECK(lo > 0, "band %d ('%s') has no minimum, so it would include unknown bitrates", b, label);
        CHECK(lo == prev_max + 1, "band %d ('%s') starts at %d, leaving a gap or overlap after %d",
              b, label, lo, prev_max);
        CHECK(hi == 0 || hi >= lo, "band %d ('%s') is inverted: %d..%d", b, label, lo, hi);

        prev_max = hi;
    }
    CHECK(prev_max == 0, "the last band must be open-ended, it ends at %d", prev_max);

    // Out of range must be printable and harmless, not NULL and not garbage.
    CHECK(strcmp(swDirBandLabel((SwBitrateBand)SW_BAND_COUNT), "") == 0, "out-of-range label");
    CHECK(strcmp(swDirBandLabel((SwBitrateBand)-1), "") == 0, "negative band label");

    lo = 7; hi = 7;
    swDirBandRange((SwBitrateBand)99, &lo, &hi);
    CHECK(lo == 0 && hi == 0, "out-of-range range should be unbounded, got %d..%d", lo, hi);

    // Both pointers optional.
    swDirBandRange(SW_BAND_HIGH, NULL, NULL);

    // The real bitrates a measured 40-station response contained were 0, 56,
    // 64, 96, 128, 192 and 320: every non-ANY band must have somewhere for one
    // of them to land, or it is a band that always comes back empty.
    const int real[] = { 56, 64, 96, 128, 192, 320 };
    for (int b = SW_BAND_LOW; b < SW_BAND_COUNT; b++) {
        swDirBandRange((SwBitrateBand)b, &lo, &hi);
        int hits = 0;
        for (size_t k = 0; k < sizeof(real) / sizeof(real[0]); k++)
            if (real[k] >= lo && (hi == 0 || real[k] <= hi)) hits++;
        CHECK(hits > 0, "band %d ('%s', %d..%d) matches none of the bitrates stations actually use",
              b, swDirBandLabel((SwBitrateBand)b), lo, hi);
    }
}

// ------------------------------------------------------------ query building

static void test_query_unfiltered(void)
{
    printf("swDirBuildQuery with no filters\n");

    SwDirFilter f;
    swDirFilterInit(&f);

    char q[SW_DIR_QUERY_MAX];
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "an empty filter must build");
    CHECK(strcmp(q, SEARCH TAIL) == 0, "got '%s'", q);

    // swDirFilterInit must actually clear a dirty struct, not just a fresh one.
    SwDirFilter dirty;
    memset(&dirty, 'x', sizeof(dirty));
    swDirFilterInit(&dirty);
    CHECK(swDirBuildQuery(&dirty, q, sizeof(q)), "a re-initialised filter must build");
    CHECK(strcmp(q, SEARCH TAIL) == 0, "swDirFilterInit left something behind: '%s'", q);

    swDirFilterInit(NULL);   // must not crash
}

static void test_query_each_filter(void)
{
    printf("swDirBuildQuery with one filter at a time\n");

    SwDirFilter f;
    char q[SW_DIR_QUERY_MAX];

    swDirFilterInit(&f);
    snprintf(f.name, sizeof(f.name), "%s", "Classic FM");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "name filter must build");
    CHECK(strcmp(q, SEARCH "name=Classic%20FM&" TAIL) == 0, "got '%s'", q);

    swDirFilterInit(&f);
    snprintf(f.tag, sizeof(f.tag), "%s", "classical");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "tag filter must build");
    CHECK(strcmp(q, SEARCH "tag=classical&" TAIL) == 0, "got '%s'", q);

    swDirFilterInit(&f);
    snprintf(f.countrycode, sizeof(f.countrycode), "%s", "GB");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "countrycode filter must build");
    CHECK(strcmp(q, SEARCH "countrycode=GB&" TAIL) == 0, "got '%s'", q);

    // A lowercase code from a saved filter must still be sent as ISO uppercase.
    swDirFilterInit(&f);
    snprintf(f.countrycode, sizeof(f.countrycode), "%s", "es");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "lowercase countrycode must build");
    CHECK(strcmp(q, SEARCH "countrycode=ES&" TAIL) == 0, "not normalised: '%s'", q);

    swDirFilterInit(&f);
    f.band = SW_BAND_HIGH;
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "band filter must build");
    CHECK(strcmp(q, SEARCH "bitrateMin=129&bitrateMax=192&" TAIL) == 0, "got '%s'", q);

    // The open-ended band must send a minimum and no maximum at all - sending
    // bitrateMax=0 would ask for stations with no bitrate.
    swDirFilterInit(&f);
    f.band = SW_BAND_TOP;
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "top band must build");
    CHECK(strcmp(q, SEARCH "bitrateMin=193&" TAIL) == 0, "got '%s'", q);
    CHECK(strstr(q, "bitrateMax") == NULL, "an open-ended band must not send bitrateMax");

    // SW_BAND_ANY must send neither bound.
    swDirFilterInit(&f);
    f.band = SW_BAND_ANY;
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "any band must build");
    CHECK(strstr(q, "bitrate") == NULL, "SW_BAND_ANY must send no bitrate bound: '%s'", q);
}

static void test_query_combined(void)
{
    printf("swDirBuildQuery with filters combined\n");

    SwDirFilter f;
    char q[SW_DIR_QUERY_MAX];

    // The realistic modal case: a search term, a country and a band together.
    swDirFilterInit(&f);
    snprintf(f.name, sizeof(f.name), "%s", "jazz");
    snprintf(f.countrycode, sizeof(f.countrycode), "%s", "ES");
    f.band = SW_BAND_MEDIUM;
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "combined filter must build");
    CHECK(strcmp(q, SEARCH "name=jazz&countrycode=ES&bitrateMin=65&bitrateMax=128&" TAIL) == 0,
          "got '%s'", q);

    // Everything at once, including both country forms.
    swDirFilterInit(&f);
    snprintf(f.name, sizeof(f.name), "%s", "radio");
    snprintf(f.tag, sizeof(f.tag), "%s", "news");
    snprintf(f.country, sizeof(f.country), "%s", "Spain");
    snprintf(f.countrycode, sizeof(f.countrycode), "%s", "ES");
    f.band = SW_BAND_LOW;
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "every filter at once must build");
    CHECK(strcmp(q, SEARCH "name=radio&country=Spain&tag=news&countrycode=ES"
                           "&bitrateMin=1&bitrateMax=64&" TAIL) == 0, "got '%s'", q);

    // Whatever the filter says, the constraints that keep the app playable must
    // survive. Sweep every band against every country in the list.
    int n = 0;
    const SwDirCountry *c = swDirCountries(&n);
    int missing = 0;
    for (int i = 0; i < n; i++) {
        for (int b = 0; b < SW_BAND_COUNT; b++) {
            swDirFilterInit(&f);
            snprintf(f.countrycode, sizeof(f.countrycode), "%s", c[i].code);
            f.band = (SwBitrateBand)b;
            if (!swDirBuildQuery(&f, q, sizeof(q))) { missing++; continue; }
            // hls=0 is the constraint that MUST survive every filter: HLS is a
            // manifest, not a stream, so no decoder makes it playable. The
            // codec constraint is deliberately NOT checked here any more - it
            // is not in the query at all, and asserting its absence is
            // test_query_does_not_pin_codec's job rather than this sweep's.
            if (!strstr(q, "hls=0") || !strstr(q, "hidebroken=true")) missing++;
        }
    }
    CHECK(missing == 0, "%d of %d country/band queries lost hls=0/hidebroken",
          missing, n * (int)SW_BAND_COUNT);
}

static void test_query_encoding(void)
{
    printf("swDirBuildQuery encodes what the user picked\n");

    SwDirFilter f;
    char q[SW_DIR_QUERY_MAX];

    // The directory's own country names carry spaces, and swDirByCountry passes
    // them straight through. This is the longest one it uses, 55 bytes.
    swDirFilterInit(&f);
    snprintf(f.country, sizeof(f.country), "%s",
             "The United Kingdom Of Great Britain And Northern Ireland");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "the longest real country name must fit");
    CHECK(strcmp(q, SEARCH "country=The%20United%20Kingdom%20Of%20Great%20Britain"
                           "%20And%20Northern%20Ireland&" TAIL) == 0, "got '%s'", q);

    // A non-ASCII country name, as /json/countries really returns it: Turkiye
    // comes back as UTF-8 "T\xC3\xBCrkiye".
    swDirFilterInit(&f);
    snprintf(f.country, sizeof(f.country), "%s", "T\xC3\xBCrkiye");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "a non-ASCII country name must build");
    CHECK(strcmp(q, SEARCH "country=T%C3%BCrkiye&" TAIL) == 0, "got '%s'", q);

    // A typed search term must never be able to add parameters of its own. If
    // '&' or '=' went through raw, "x&hls=1" would sit after the tail's hls=0
    // and radio-browser takes the LAST value for a repeated parameter - so a
    // station name typed by the user could turn HLS results back on and fill
    // the list with manifests nothing can play. This is a hostile-input test:
    // the directory is community-editable and the search box is free text.
    swDirFilterInit(&f);
    snprintf(f.name, sizeof(f.name), "%s", "x&codec=AAC&hls=1");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "must build");
    CHECK(strcmp(q, SEARCH "name=x%26codec%3DAAC%26hls%3D1&" TAIL) == 0, "got '%s'", q);
    CHECK(strstr(q, "codec=AAC") == NULL, "a search term injected a parameter: '%s'", q);
    CHECK(strstr(q, "hls=1") == NULL, "a search term re-enabled HLS: '%s'", q);

    // '#' would truncate the whole query at the server.
    swDirFilterInit(&f);
    snprintf(f.tag, sizeof(f.tag), "%s", "drum#bass");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "must build");
    CHECK(strcmp(q, SEARCH "tag=drum%23bass&" TAIL) == 0, "got '%s'", q);

    // A tag with an apostrophe and a plus, both of which have to survive as
    // themselves rather than becoming a space.
    swDirFilterInit(&f);
    snprintf(f.tag, sizeof(f.tag), "%s", "rock'n'roll+");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "must build");
    CHECK(strcmp(q, SEARCH "tag=rock%27n%27roll%2B&" TAIL) == 0, "got '%s'", q);
}

static void test_query_refuses_rather_than_truncating(void)
{
    printf("swDirBuildQuery refuses a query it cannot build whole\n");

    SwDirFilter f;
    char q[SW_DIR_QUERY_MAX];

    swDirFilterInit(&f);
    CHECK(!swDirBuildQuery(NULL, q, sizeof(q)), "NULL filter must be refused");
    CHECK(!swDirBuildQuery(&f, NULL, 16), "NULL buffer must be refused");
    CHECK(!swDirBuildQuery(&f, q, 0), "a zero-length buffer must be refused");

    // A buffer that fits the path but not the tail. The point is that `q` comes
    // back empty rather than holding a query that would search for the wrong
    // thing.
    char small[40];
    memset(small, 'Z', sizeof(small));
    CHECK(!swDirBuildQuery(&f, small, sizeof(small)), "a short buffer must be refused");
    CHECK(small[0] == 0, "a refused build must leave the buffer empty, not partial");

    // A country name long enough that its encoded form overruns.
    swDirFilterInit(&f);
    memset(f.country, 0xE2, sizeof(f.country) - 1);   // 79 bytes, all escaped 3x
    f.country[sizeof(f.country) - 1] = 0;
    char medium[128];
    CHECK(!swDirBuildQuery(&f, medium, sizeof(medium)),
          "an over-long encoded value must be refused, not clipped");
    CHECK(medium[0] == 0, "refused build left '%s'", medium);

    // ISO alpha-3 is not reachable at all: countrycode is SW_DIR_CC_LEN bytes,
    // so `snprintf(f.countrycode, sizeof(f.countrycode), "%s", "GBR")` does not
    // even compile under -Werror=format-truncation. The field width, not the
    // validation below, is what stops that mistake.

    // A countrycode that did not come from swDirCountries().
    const char *bogus[] = { "G", "1B", "G1", "  ", "-", "%2", "g!" };
    for (size_t i = 0; i < sizeof(bogus) / sizeof(bogus[0]); i++) {
        swDirFilterInit(&f);
        snprintf(f.countrycode, sizeof(f.countrycode), "%s", bogus[i]);
        CHECK(!swDirBuildQuery(&f, q, sizeof(q)),
              "countrycode '%s' should be refused, built '%s'", bogus[i], q);
    }

    // An unterminated countrycode must not be read past.
    swDirFilterInit(&f);
    memset(f.countrycode, 'A', sizeof(f.countrycode));
    CHECK(!swDirBuildQuery(&f, q, sizeof(q)), "an unterminated countrycode must be refused");

    // An unterminated text field likewise.
    swDirFilterInit(&f);
    memset(f.name, 'A', sizeof(f.name));
    CHECK(!swDirBuildQuery(&f, q, sizeof(q)), "an unterminated name must be refused");

    // A band outside the enum - a saved filter from a future version, say.
    swDirFilterInit(&f);
    f.band = (SwBitrateBand)SW_BAND_COUNT;
    CHECK(!swDirBuildQuery(&f, q, sizeof(q)), "an out-of-range band must be refused");

    swDirFilterInit(&f);
    f.band = (SwBitrateBand)-3;
    CHECK(!swDirBuildQuery(&f, q, sizeof(q)), "a negative band must be refused");
}

// ------------------------------------------------------------------- parsing

// Three stations copied verbatim out of real API responses on 2026-09-06:
//   http://de1.api.radio-browser.info/json/stations/search
//       ?codec=MP3&hls=0&hidebroken=true&order=clickcount&reverse=true
//       &limit=40&countrycode=GB&bitrateMin=128&bitrateMax=192
// and the same with countrycode=ES&bitrateMin=64&bitrateMax=96.
//
// These are here rather than in the synthetic fixture above because the filter
// work depends on two fields the synthetic one could always be wrong about:
// `bitrate` and `country`. A bitrate filter over a field that is never
// populated returns nothing and looks exactly like "no stations matched".
static const char *kReal =
"["
"{\"changeuuid\":\"460e17da-5b33-41e1-92cf-177a8ecfdb81\",\"stationuuid\":\"96063f25-0601"
"-11e8-ae97-52543be04c81\",\"serveruuid\":null,\"name\":\"Classic FM UK\",\"url\":\"http:"
"//ice-the.musicradio.com/ClassicFMMP3\",\"url_resolved\":\"http://ice-the.musicradio.com"
"/ClassicFMMP3\",\"homepage\":\"http://www.classicfm.com/\",\"favicon\":\"http://www.clas"
"sicfm.com/assets_v4r/classic/img/favicon-196x196.png\",\"tags\":\"classical\",\"country"
"\":\"The United Kingdom Of Great Britain And Northern Ireland\",\"countrycode\":\"GB\",\""
"iso_3166_2\":\"\",\"state\":\"London\",\"language\":\"english\",\"languagecodes\":\"en\""
",\"votes\":56851,\"lastchangetime\":\"2026-01-15 05:47:23\",\"lastchangetime_iso8601\":"
"\"2026-01-15T05:47:23Z\",\"codec\":\"MP3\",\"bitrate\":128,\"hls\":0,\"lastcheckok\":1,\""
"lastchecktime\":\"2026-09-05 20:39:08\",\"lastchecktime_iso8601\":\"2026-09-05T20:39:08Z"
"\",\"lastcheckoktime\":\"2026-09-05 20:39:08\",\"lastcheckoktime_iso8601\":\"2026-09-05T"
"20:39:08Z\",\"lastlocalchecktime\":\"2026-09-05 20:39:08\",\"lastlocalchecktime_iso8601"
"\":\"2026-09-05T20:39:08Z\",\"clicktimestamp\":\"2026-09-06 21:50:49\",\"clicktimestamp_i"
"so8601\":\"2026-09-06T21:50:49Z\",\"clickcount\":160,\"clicktrend\":160,\"ssl_error\":0,"
"\"geo_lat\":null,\"geo_long\":null,\"geo_distance\":null,\"has_extended_info\":false},"

"{\"changeuuid\":\"598c82b2-b008-420e-a8ce-9a8b9f491a51\",\"stationuuid\":\"d69fbc2c-530a"
"-4d40-ac9f-56532356b248\",\"serveruuid\":null,\"name\":\"Chillout Ibiza FM\",\"url\":\"h"
"ttps://edge3.peta.live365.net/b05055_128mp3\",\"url_resolved\":\"https://edge3.peta.live"
"365.net/b05055_128mp3\",\"homepage\":\"http://www.chilloutibizafm.com/\",\"favicon\":\"h"
"ttp://www.chilloutibizafm.com/favicon.ico\",\"tags\":\"chillout,relaxing\",\"country\":"
"\"The United Kingdom Of Great Britain And Northern Ireland\",\"countrycode\":\"GB\",\"iso"
"_3166_2\":\"\",\"state\":\"\",\"language\":\"english\",\"languagecodes\":\"en\",\"votes"
"\":3239,\"lastchangetime\":\"2026-01-15 03:18:28\",\"lastchangetime_iso8601\":\"2026-01-1"
"5T03:18:28Z\",\"codec\":\"MP3\",\"bitrate\":192,\"hls\":0,\"lastcheckok\":1,\"lastcheckt"
"ime\":\"2026-09-06 08:29:59\",\"lastchecktime_iso8601\":\"2026-09-06T08:29:59Z\",\"lastc"
"heckoktime\":\"2026-09-06 08:29:59\",\"lastcheckoktime_iso8601\":\"2026-09-06T08:29:59Z"
"\",\"lastlocalchecktime\":\"2026-09-06 08:29:59\",\"lastlocalchecktime_iso8601\":\"2026-0"
"9-06T08:29:59Z\",\"clicktimestamp\":\"2026-09-06 21:35:26\",\"clicktimestamp_iso8601\":"
"\"2026-09-06T21:35:26Z\",\"clickcount\":48,\"clicktrend\":48,\"ssl_error\":0,\"geo_lat\":"
"null,\"geo_long\":null,\"geo_distance\":null,\"has_extended_info\":false},"

"{\"changeuuid\":\"76f45de0-a7e5-4cc0-b264-e27ce0299b14\",\"stationuuid\":\"9584da46-3ab4"
"-11e9-9b4e-52543be04c81\",\"serveruuid\":null,\"name\":\"COPE M\\u00e1laga\",\"url\":\"h"
"ttp://net1.cope.stream.flumotion.com/cope/net1.mp3.m3u\",\"url_resolved\":\"http://fluca"
"st34-h-cloud.flumotion.com/cope/net1.mp3\",\"homepage\":\"https://www.cope.es/directos/m"
"alaga\",\"favicon\":\"https://www.cope.es/favicon/cope/apple-touch-icon-192x192.png\",\""
"tags\":\"\",\"country\":\"Spain\",\"countrycode\":\"ES\",\"iso_3166_2\":\"\",\"state\":"
"\"M\\u00e1laga\",\"language\":\"\",\"languagecodes\":\"\",\"votes\":1152,\"lastchangetime"
"\":\"2026-01-14 22:54:07\",\"lastchangetime_iso8601\":\"2026-01-14T22:54:07Z\",\"codec\""
":\"MP3\",\"bitrate\":96,\"hls\":0,\"lastcheckok\":1,\"lastchecktime\":\"2026-09-06 10:37"
":09\",\"lastchecktime_iso8601\":\"2026-09-06T10:37:09Z\",\"lastcheckoktime\":\"2026-09-0"
"6 10:37:09\",\"lastcheckoktime_iso8601\":\"2026-09-06T10:37:09Z\",\"lastlocalchecktime\""
":\"2026-09-06 10:37:09\",\"lastlocalchecktime_iso8601\":\"2026-09-06T10:37:09Z\",\"click"
"timestamp\":\"2026-09-06 21:41:36\",\"clicktimestamp_iso8601\":\"2026-09-06T21:41:36Z\","
"\"clickcount\":50,\"clicktrend\":50,\"ssl_error\":0,\"geo_lat\":null,\"geo_long\":null,"
"\"geo_distance\":null,\"has_extended_info\":false}"
"]";

static void test_filter_fields_are_populated(void)
{
    printf("the fields the filter relies on survive a real response\n");

    SwStationList l;
    SwDirResult r = swDirParseJson(kReal, strlen(kReal), &l);

    CHECK(r == SW_DIR_OK, "real captured stations did not parse: %s", swDirErrorText(r));
    CHECK(l.count == 3, "expected 3 real stations, got %d", l.count);
    if (l.count != 3) return;

    // If bitrate came back 0 here, every band but SW_BAND_ANY would quietly
    // return nothing on hardware and look like a dead directory.
    CHECK(l.items[0].bitrate == 128, "bitrate[0]=%d, expected 128", l.items[0].bitrate);
    CHECK(l.items[1].bitrate == 192, "bitrate[1]=%d, expected 192", l.items[1].bitrate);
    CHECK(l.items[2].bitrate == 96,  "bitrate[2]=%d, expected 96",  l.items[2].bitrate);

    // Each real bitrate must land in the band the user would pick for it.
    const SwBitrateBand expect[3] = { SW_BAND_MEDIUM, SW_BAND_HIGH, SW_BAND_MEDIUM };
    for (int i = 0; i < 3; i++) {
        int lo = 0, hi = 0;
        swDirBandRange(expect[i], &lo, &hi);
        CHECK(l.items[i].bitrate >= lo && (hi == 0 || l.items[i].bitrate <= hi),
              "%d kbps does not fall in '%s' (%d..%d)",
              l.items[i].bitrate, swDirBandLabel(expect[i]), lo, hi);
    }

    // The long UK country name is exactly the case SW_STATION_CC is too small
    // for; it must be clipped cleanly rather than left unterminated, and it is
    // why the filter sends countrycode instead of this string.
    CHECK(strlen(l.items[0].country) == SW_STATION_CC - 1,
          "the 55-byte country name should fill the field, len=%zu", strlen(l.items[0].country));
    CHECK(strncmp(l.items[0].country, "The United Kingdom", 18) == 0,
          "country[0]='%s'", l.items[0].country);

    CHECK(strcmp(l.items[2].country, "Spain") == 0, "country[2]='%s'", l.items[2].country);

    // The Spanish station's name carries a \u escape in the real feed.
    CHECK(strcmp(l.items[2].name, "COPE M\xC3\xA1laga") == 0,
          "real \\u escape not decoded: '%s'", l.items[2].name);

    // And the station the ES query returned really is one the built-in list can
    // ask for, which is the whole point of shipping codes rather than names.
    CHECK(swDirCountryIndex("ES") > 0, "ES is not in the country list");
    CHECK(swDirCountryIndex("GB") > 0, "GB is not in the country list");
}

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

// The codec whitelist. This exists because `codec=MP3` used to be pinned into
// QUERY_TAIL and the server applied it, which meant the AAC decoder shipped
// unreachable - no AAC station could ever come back from a query to be handed
// to it. The constraint now lives in the parser so it can express both codecs,
// and this is what proves it still refuses the ones there is no decoder for.
static const char *kCodecs =
"["
 "{\"name\":\"Mp3 Station\",\"url_resolved\":\"http://a/\",\"codec\":\"MP3\"},"
 "{\"name\":\"Aac Station\",\"url_resolved\":\"http://b/\",\"codec\":\"AAC\"},"
 "{\"name\":\"AacPlus Station\",\"url_resolved\":\"http://c/\",\"codec\":\"AAC+\"},"
 "{\"name\":\"Aacp Station\",\"url_resolved\":\"http://d/\",\"codec\":\"AACP\"},"
 // Case is not guaranteed by the API, and a case-sensitive compare would drop
 // real stations silently.
 "{\"name\":\"Lowercase Aac\",\"url_resolved\":\"http://e/\",\"codec\":\"aac\"},"
 // No codec key at all. Kept: every real response carries the field, so an
 // absent one means a hand-built object, and dropping it would fail fixtures
 // for a reason that has nothing to do with codecs.
 "{\"name\":\"No Codec Key\",\"url_resolved\":\"http://f/\"},"
 // The four that must be refused. UNKNOWN is deliberately here: the directory
 // saying it does not know is not permission to guess on the user's behalf.
 "{\"name\":\"Ogg Station\",\"url_resolved\":\"http://g/\",\"codec\":\"OGG\"},"
 "{\"name\":\"Opus Station\",\"url_resolved\":\"http://h/\",\"codec\":\"OPUS\"},"
 "{\"name\":\"Flac Station\",\"url_resolved\":\"http://i/\",\"codec\":\"FLAC\"},"
 "{\"name\":\"Unknown Station\",\"url_resolved\":\"http://j/\",\"codec\":\"UNKNOWN\"},"
 // A prefix of an accepted value, and an accepted value with a suffix. Both
 // must be refused - a substring or prefix compare would let these through.
 "{\"name\":\"Aa Station\",\"url_resolved\":\"http://k/\",\"codec\":\"AA\"},"
 "{\"name\":\"Mp3x Station\",\"url_resolved\":\"http://l/\",\"codec\":\"MP3X\"}"
"]";

static void test_parse_codec_filter(void)
{
    printf("only codecs with a decoder survive parsing\n");

    SwStationList l;
    SwDirResult r = swDirParseJson(kCodecs, strlen(kCodecs), &l);

    CHECK(r == SW_DIR_OK, "result was %d", (int)r);
    CHECK(l.count == 6, "expected 6 playable of 12, got %d", l.count);

    // Named rather than counted, so a wrong six is still a failure.
    static const char *want[] = {
        "Mp3 Station", "Aac Station", "AacPlus Station",
        "Aacp Station", "Lowercase Aac", "No Codec Key",
    };
    for (int i = 0; i < l.count && i < 6; i++)
        CHECK(strcmp(l.items[i].name, want[i]) == 0,
              "kept[%d]='%s', want '%s'", i, l.items[i].name, want[i]);

    // And the refused ones must not appear anywhere in the list at all.
    static const char *never[] = {
        "Ogg Station", "Opus Station", "Flac Station",
        "Unknown Station", "Aa Station", "Mp3x Station",
    };
    for (size_t k = 0; k < sizeof(never) / sizeof(never[0]); k++) {
        bool found = false;
        for (int i = 0; i < l.count; i++)
            if (strcmp(l.items[i].name, never[k]) == 0) found = true;
        CHECK(!found, "%s has no decoder and must not be offered", never[k]);
    }
}

// The query must NOT pin a codec any more. If codec=MP3 ever comes back, every
// AAC station silently disappears again and nothing else in the suite notices -
// the parser filter above would still pass, because it would simply never be
// handed an AAC station to keep.
static void test_query_does_not_pin_codec(void)
{
    printf("the fixed query tail leaves codec to the parser, but still bars HLS\n");

    SwDirFilter f;
    swDirFilterInit(&f);

    char q[SW_DIR_QUERY_MAX];
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "query build failed");
    CHECK(strstr(q, "codec=") == NULL,
          "query must not constrain codec, got \"%s\"", q);
    CHECK(strstr(q, "hls=0") != NULL,
          "hls=0 must stay - HLS is unplayable whatever the codec: \"%s\"", q);
    CHECK(strstr(q, "hidebroken=true") != NULL, "hidebroken lost: \"%s\"", q);
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

    // The bitrate bands are only meaningful if the directory really fills this
    // in. Measured on a saved 40-station response: 34 carried a bitrate and 6
    // reported 0. If a whole real response came back all-zero, every band but
    // SW_BAND_ANY would return nothing and be indistinguishable from an outage.
    int with_bitrate = 0, in_a_band = 0;
    for (int i = 0; i < l.count; i++) {
        if (l.items[i].bitrate <= 0) continue;
        with_bitrate++;

        for (int b = SW_BAND_LOW; b < SW_BAND_COUNT; b++) {
            int lo = 0, hi = 0;
            swDirBandRange((SwBitrateBand)b, &lo, &hi);
            if (l.items[i].bitrate >= lo && (hi == 0 || l.items[i].bitrate <= hi)) {
                in_a_band++;
                break;
            }
        }
    }
    printf("    %d of %d stations report a bitrate\n", with_bitrate, l.count);
    CHECK(with_bitrate > 0, "no station in a real response carried a bitrate - "
                            "every bitrate band would come back empty");
    CHECK(in_a_band == with_bitrate,
          "%d of %d real bitrates fall outside every band", with_bitrate - in_a_band, with_bitrate);

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

    test_country_list();
    test_country_index();
    test_bitrate_bands();
    test_query_unfiltered();
    test_query_each_filter();
    test_query_combined();
    test_query_encoding();
    test_query_refuses_rather_than_truncating();
    test_filter_fields_are_populated();

    test_parse_good();
    test_parse_codec_filter();
    test_query_does_not_pin_codec();
    test_parse_rejects_junk();
    test_overlong_fields_are_truncated_safely();
    test_caps_at_station_max();

    printf("real API response\n");
    test_real_response(argc > 1 ? argv[1] : "real.json");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
