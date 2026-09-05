#include "favourites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FAV_DIR  "sdmc:/3ds/skywave"
#define FAV_PATH FAV_DIR "/favourites.tsv"

static SwStationList g_list;
static bool          g_loaded;

// ------------------------------------------------------------------ matching

// Two records are the same station if the directory gives them the same UUID.
// Some entries have no UUID, so the URL is the fallback - it is what actually
// gets played, so two rows with the same URL are the same station whatever they
// are called.
static bool same_station(const SwStation *a, const SwStation *b)
{
    if (a->uuid[0] && b->uuid[0]) return strcmp(a->uuid, b->uuid) == 0;
    return strcmp(a->url, b->url) == 0;
}

static int index_of(const SwStation *st)
{
    for (int i = 0; i < g_list.count; i++)
        if (same_station(&g_list.items[i], st)) return i;
    return -1;
}

// ---------------------------------------------------------------------- file

// Copies `src` into `dst` with tabs and newlines turned into spaces. A station
// name containing a tab would silently shift every later field on the line, so
// it is removed on the way in rather than guarded against on the way out.
static void sanitise(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap; p++)
        dst[o++] = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
    if (cap) dst[o] = 0;
}

// Reads one tab-delimited field out of `*p`, advancing it past the delimiter.
// Returns false at the end of the line.
static bool next_field(char **p, char *dst, size_t cap)
{
    char *s = *p;
    if (!s || !*s) return false;

    char *end = s;
    while (*end && *end != '\t' && *end != '\n' && *end != '\r') end++;

    size_t len = (size_t)(end - s);
    if (len >= cap) len = cap - 1;
    memcpy(dst, s, len);
    dst[len] = 0;

    if (*end == '\t') end++;
    *p = end;
    return true;
}

static void save(void)
{
    mkdir(FAV_DIR, 0777);   // harmless if it already exists

    FILE *f = fopen(FAV_PATH, "w");
    if (!f) return;         // a full or absent SD card is not worth a dialog

    for (int i = 0; i < g_list.count; i++) {
        const SwStation *s = &g_list.items[i];

        char name[SW_STATION_NAME], url[SW_STATION_URL];
        char uuid[SW_STATION_UUID], country[SW_STATION_CC];
        sanitise(name,    sizeof(name),    s->name);
        sanitise(url,     sizeof(url),     s->url);
        sanitise(uuid,    sizeof(uuid),    s->uuid);
        sanitise(country, sizeof(country), s->country);

        fprintf(f, "%s\t%s\t%s\t%s\t%d\n", name, url, uuid, country, s->bitrate);
    }

    fclose(f);
}

void swFavLoad(void)
{
    memset(&g_list, 0, sizeof(g_list));
    g_loaded = true;

    FILE *f = fopen(FAV_PATH, "r");
    if (!f) return;

    char line[SW_STATION_NAME + SW_STATION_URL + SW_STATION_UUID + SW_STATION_CC + 32];

    while (g_list.count < SW_STATIONS_MAX && fgets(line, sizeof(line), f)) {
        SwStation st;
        memset(&st, 0, sizeof(st));

        char *p = line;
        char bitrate[16] = {0};

        if (!next_field(&p, st.name,    sizeof(st.name)))    continue;
        if (!next_field(&p, st.url,     sizeof(st.url)))     continue;
        next_field(&p, st.uuid,    sizeof(st.uuid));
        next_field(&p, st.country, sizeof(st.country));
        if (next_field(&p, bitrate, sizeof(bitrate))) st.bitrate = atoi(bitrate);

        // A line that lost its URL - a truncated write, a hand edit gone wrong -
        // would show as a row that plays nothing, so it is dropped instead.
        if (!st.name[0] || !st.url[0]) continue;

        g_list.items[g_list.count++] = st;
    }

    fclose(f);
}

// ------------------------------------------------------------------- queries

const SwStationList *swFavList(void)
{
    if (!g_loaded) swFavLoad();
    return &g_list;
}

bool swFavContains(const SwStation *st)
{
    if (!g_loaded) swFavLoad();
    return st && index_of(st) >= 0;
}

// ------------------------------------------------------------------- editing

bool swFavAdd(const SwStation *st)
{
    if (!g_loaded) swFavLoad();
    if (!st || !st->url[0]) return false;
    if (index_of(st) >= 0) return false;
    if (g_list.count >= SW_STATIONS_MAX) return false;

    g_list.items[g_list.count++] = *st;
    save();
    return true;
}

bool swFavRemove(const SwStation *st)
{
    if (!g_loaded) swFavLoad();
    if (!st) return false;

    int i = index_of(st);
    if (i < 0) return false;

    for (int k = i; k < g_list.count - 1; k++) g_list.items[k] = g_list.items[k + 1];
    g_list.count--;
    save();
    return true;
}

bool swFavToggle(const SwStation *st)
{
    if (swFavContains(st)) { swFavRemove(st); return false; }
    return swFavAdd(st);
}
