#include "presets.h"

#include <stdio.h>
#include <string.h>

// The table is its own type rather than an array of SwStation, and that is the
// point of it: an SwStation carries a 320-byte URL buffer and a 72-byte name
// buffer whether or not the strings need the room, so 40 of them would be about
// 18 KB of mostly-padding baked into the binary. Pointers to string literals
// cost the bytes the strings actually use.
//
// The country is a short, plain name - "United Kingdom", not the directory's
// "The United Kingdom Of Great Britain And Northern Ireland", which is 54 bytes
// and would not even fit SW_STATION_CC. It is here to group and filter the list
// on screen, so short and readable beats matching the directory's spelling.
//
// Bitrates are the icy-br each server reported when the stream was checked, not
// a number copied from a listings page.
typedef struct {
    const char *name;
    const char *url;
    const char *country;
    int         bitrate;
} Preset;

// Ordered UK first, then the rest by country, because that is the order they
// will be read in: the list was asked for to put British stations in reach, and
// a user scrolling past forty entries wants them grouped, not shuffled.
//
// ASCII only, including in names that are properly accented - "RTE" rather than
// "RTE" with the acute. The console's font and this file's encoding are two
// places a non-ASCII byte can go wrong on the way to the screen, and a station
// name is not worth finding out which.
static const Preset kPresets[] = {
    // ---- United Kingdom ---------------------------------------------------
    { "Heart UK",             "http://media-ice.musicradio.com/HeartUKMP3",            "United Kingdom", 128 },
    { "Heart 80s",            "http://media-ice.musicradio.com/Heart80sMP3",           "United Kingdom", 128 },
    { "Heart 90s",            "http://media-ice.musicradio.com/Heart90sMP3",           "United Kingdom", 128 },
    { "Capital FM",           "http://media-ice.musicradio.com/CapitalMP3",            "United Kingdom", 128 },
    { "Capital XTRA",         "http://media-the.musicradio.com/CapitalXTRALondonMP3",  "United Kingdom", 128 },
    { "Capital Dance",        "http://icecast.thisisdax.com/CapitalDanceMP3",          "United Kingdom", 128 },
    { "Classic FM",           "http://media-ice.musicradio.com/ClassicFMMP3",          "United Kingdom", 128 },
    { "Smooth UK",            "http://media-ice.musicradio.com/SmoothUKMP3",           "United Kingdom", 128 },
    { "Smooth Chill",         "http://media-ice.musicradio.com/ChillMP3",              "United Kingdom", 128 },
    { "Radio X",              "http://icecast.thisisdax.com/RadioXUKMP3",              "United Kingdom", 128 },
    { "Radio X Classic Rock", "http://media-ice.musicradio.com/RadioXClassicRockMP3",  "United Kingdom", 128 },
    { "Gold",                 "http://media-ice.musicradio.com/GoldMP3",               "United Kingdom", 128 },
    // LBC and LBC News are both 48 kbps: that is the only rate Global publishes
    // these two on as MP3, not a low-quality variant picked by mistake.
    { "LBC",                  "http://media-ice.musicradio.com/LBCLondonMP3",          "United Kingdom",  48 },
    { "LBC News",             "http://media-ice.musicradio.com/LBCUKMP3",              "United Kingdom",  48 },
    { "GB News Radio",        "http://listen-gbnews.sharp-stream.com/gbnews.mp3",      "United Kingdom", 128 },
    { "NTS Radio 1",          "http://stream-relay-geo.ntslive.net/stream",            "United Kingdom", 256 },
    { "NTS Radio 2",          "http://stream-relay-geo.ntslive.net/stream2",           "United Kingdom", 256 },
    // The only BBC service still on plain MP3 - see the header for the rest.
    { "BBC World Service",    "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service", "United Kingdom", 56 },

    // ---- Ireland ----------------------------------------------------------
    { "RTE Radio 1",          "http://icecast.rte.ie/radio1",                          "Ireland",        160 },
    { "RTE 2FM",              "http://icecast.rte.ie/2fm",                             "Ireland",        160 },

    // ---- United States ----------------------------------------------------
    { "SomaFM Groove Salad",  "http://ice1.somafm.com/groovesalad-128-mp3",            "United States",  128 },
    { "SomaFM Drone Zone",    "http://ice1.somafm.com/dronezone-128-mp3",              "United States",  128 },
    { "SomaFM Indie Pop",     "http://ice1.somafm.com/indiepop-128-mp3",               "United States",  128 },
    { "SomaFM Secret Agent",  "http://ice1.somafm.com/secretagent-128-mp3",            "United States",  128 },
    { "SomaFM Underground 80s", "http://ice1.somafm.com/u80s-128-mp3",                 "United States",  128 },
    { "Radio Paradise",       "http://stream.radioparadise.com/mp3-192",               "United States",  192 },
    { "KEXP 90.3",            "http://kexp-mp3-128.streamguys1.com/kexp128.mp3",       "United States",  128 },
    { "WFMU",                 "http://stream0.wfmu.org/freeform-128k",                 "United States",  128 },
    { "WNYC-FM",              "http://fm939.wnyc.org/wnycfm",                          "United States",   96 },

    // ---- Switzerland ------------------------------------------------------
    { "Radio Swiss Jazz",     "http://stream.srg-ssr.ch/m/rsj/mp3_128",                "Switzerland",    128 },
    { "Radio Swiss Classic",  "http://stream.srg-ssr.ch/m/rsc_de/mp3_128",             "Switzerland",    128 },
    { "Radio Swiss Pop",      "http://stream.srg-ssr.ch/m/rsp/mp3_128",                "Switzerland",    128 },

    // ---- France -----------------------------------------------------------
    { "FIP",                  "http://icecast.radiofrance.fr/fip-midfi.mp3",           "France",         128 },
    { "France Inter",         "http://icecast.radiofrance.fr/franceinter-midfi.mp3",   "France",         128 },
    { "France Musique",       "http://icecast.radiofrance.fr/francemusique-midfi.mp3", "France",         128 },

    // ---- Germany ----------------------------------------------------------
    // Answers with a redirect carrying a signed, expiring token. The entry URL
    // is the stable one, so it is the one stored - the stream client follows
    // the redirect at play time and gets a fresh token each session.
    { "Deutschlandfunk",      "http://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3", "Germany",       128 },
    { "Antenne Bayern",       "http://stream.antenne.de/antenne",                      "Germany",        128 },

    // ---- Netherlands ------------------------------------------------------
    { "NPO Radio 2",          "http://icecast.omroep.nl/radio2-bb-mp3",                "Netherlands",    192 },

    // ---- Italy ------------------------------------------------------------
    { "Rai Radio 3",          "http://icestreaming.rai.it/3.mp3",                      "Italy",          320 },
    { "Virgin Radio Italia",  "http://icecast.unitedradio.it/Virgin.mp3",              "Italy",          128 },
};

#define PRESET_COUNT ((int)(sizeof(kPresets) / sizeof(kPresets[0])))

// The header promises a caller can page these into an SwStationList. That is
// only true while the list stays inside the directory's cap, and a list of
// stations is exactly the kind of thing someone appends one more line to
// without counting - so the promise is checked by the compiler rather than left
// to whoever edits the table next.
_Static_assert(PRESET_COUNT <= SW_STATIONS_MAX,
               "presets no longer fit an SwStationList - see swPresetCount");

static bool in_range(int index)
{
    return index >= 0 && index < PRESET_COUNT;
}

int swPresetCount(void)
{
    return PRESET_COUNT;
}

bool swPresetGet(int index, SwStation *out)
{
    if (!out || !in_range(index)) return false;

    const Preset *p = &kPresets[index];

    // Zeroed first rather than field by field, so the uuid this list has no
    // value for ends up empty by construction and a field added to SwStation
    // later arrives zeroed instead of holding whatever was on the caller's
    // stack.
    memset(out, 0, sizeof(*out));

    // The strings are shorter than the fields they go into - checked against
    // SW_STATION_NAME and SW_STATION_URL when the table was written - but the
    // bounded copies stay, because the next entry added to the table will not
    // be checked by anything.
    snprintf(out->name,    sizeof(out->name),    "%s", p->name);
    snprintf(out->url,     sizeof(out->url),     "%s", p->url);
    snprintf(out->country, sizeof(out->country), "%s", p->country);
    out->bitrate = p->bitrate;

    return true;
}

const char *swPresetName(int index)
{
    return in_range(index) ? kPresets[index].name : "";
}

const char *swPresetCountry(int index)
{
    return in_range(index) ? kPresets[index].country : "";
}

int swPresetBitrate(int index)
{
    return in_range(index) ? kPresets[index].bitrate : 0;
}
