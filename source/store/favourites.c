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

// Writes the whole list to a sibling temp file and renames it over the real
// one, rather than opening FAV_PATH with "w" and writing into it directly.
// Opening with "w" truncates the old file to zero bytes before a single new
// byte is written, so a crash, a killed app or a pulled battery mid-write
// used to leave an empty or half-written favourites.tsv - the worst possible
// outcome, since it looks like a clean save right up until the next launch.
// rename() onto an existing path is a single directory-entry swap, so the
// old file is still intact right up until the new one is fully written and
// closed; a failure at any point before the rename leaves the previous
// favourites exactly as they were.
//
// Returns false if anything along the way failed, so the caller can undo an
// in-memory change instead of reporting success for a save that never
// reached the SD card.
static bool save(void)
{
    mkdir(FAV_DIR, 0777);   // harmless if it already exists

    const char *tmp_path = FAV_PATH ".tmp";

    FILE *f = fopen(tmp_path, "w");
    if (!f) return false;   // full or absent SD card - let the caller report it

    bool ok = true;

    for (int i = 0; i < g_list.count; i++) {
        const SwStation *s = &g_list.items[i];

        char name[SW_STATION_NAME], url[SW_STATION_URL];
        char uuid[SW_STATION_UUID], country[SW_STATION_CC];
        sanitise(name,    sizeof(name),    s->name);
        sanitise(url,     sizeof(url),     s->url);
        sanitise(uuid,    sizeof(uuid),    s->uuid);
        sanitise(country, sizeof(country), s->country);

        if (fprintf(f, "%s\t%s\t%s\t%s\t%d\n", name, url, uuid, country, s->bitrate) < 0)
            ok = false;   // a short/failed write - typically a full SD card
    }

    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;

    if (!ok) {
        remove(tmp_path);   // don't leave a half-written temp file behind
        return false;
    }

    if (rename(tmp_path, FAV_PATH) != 0) {
        remove(tmp_path);
        return false;
    }

    return true;
}

void swFavLoad(void)
{
    memset(&g_list, 0, sizeof(g_list));
    g_loaded = true;

    FILE *f = fopen(FAV_PATH, "r");
    if (!f) return;

    char line[SW_STATION_NAME + SW_STATION_URL + SW_STATION_UUID + SW_STATION_CC + 32];

    while (g_list.count < SW_STATIONS_MAX) {
        long start = ftell(f);
        if (!fgets(line, sizeof(line), f)) break;

        // save() always ends every row with '\n', so in a well-formed file
        // every physical line - including the last - has one. A line with
        // none is damaged in one of two ways, both dropped rather than
        // parsed as a fragment:
        //
        //  - It fills this whole buffer with no newline in it: a field
        //    longer than any legitimate one, or plain corruption. Left
        //    alone, the next fgets call would resume mid-field and misread
        //    its tail as a brand new record, so the rest of the physical
        //    line is eaten here first.
        //  - It simply runs out at EOF: the write (or the SD card) was cut
        //    off mid-line. Whatever fields did make it through are a partial
        //    picture - e.g. a URL truncated to something that still looks
        //    non-empty but isn't the real address - so the row is dropped
        //    outright rather than trusted.
        //
        // The byte count comes from ftell, not strlen/strchr on the buffer,
        // which an embedded NUL could fool into stopping early.
        long end = ftell(f);
        size_t nread = (start >= 0 && end > start) ? (size_t)(end - start) : 0;
        bool complete_line = nread > 0 && line[nread - 1] == '\n';

        if (!complete_line) {
            if (nread == sizeof(line) - 1 && !feof(f)) {
                int c;
                while ((c = fgetc(f)) != EOF && c != '\n') {}
            }
            continue;
        }

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

    if (!save()) {
        // The write didn't reach the SD card (full/removed card, I/O error).
        // Undo the in-memory add so the running list matches what is
        // actually on disk - otherwise the star shows "saved" for the rest
        // of this session and the station is just gone on the next launch,
        // with the user never told either time.
        g_list.count--;
        return false;
    }

    return true;
}

bool swFavRemove(const SwStation *st)
{
    if (!g_loaded) swFavLoad();
    if (!st) return false;

    int i = index_of(st);
    if (i < 0) return false;

    SwStation removed = g_list.items[i];
    for (int k = i; k < g_list.count - 1; k++) g_list.items[k] = g_list.items[k + 1];
    g_list.count--;

    if (!save()) {
        // Couldn't persist the removal - put it back so memory keeps
        // matching disk, the same reasoning as the rollback in swFavAdd.
        for (int k = g_list.count; k > i; k--) g_list.items[k] = g_list.items[k - 1];
        g_list.items[i] = removed;
        g_list.count++;
        return false;
    }

    return true;
}

bool swFavToggle(const SwStation *st)
{
    // swFavRemove can now fail and roll itself back (a save that couldn't
    // reach the SD card), so "removed" no longer means "definitely gone" -
    // report what's actually true afterwards rather than assuming the
    // removal always lands.
    if (swFavContains(st)) return !swFavRemove(st);
    return swFavAdd(st);
}
