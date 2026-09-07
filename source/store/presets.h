#pragma once

// The built-in station library: a fixed list of well-known stations that ships
// inside the app.
//
// This deliberately contradicts the note at the top of directory.h, which says
// a station list in a header file goes stale. It does, and that reasoning still
// holds for the *directory*. The presets exist anyway, for the case the
// directory cannot cover: it is a network dependency, and a radio whose first
// screen is an error because radio-browser is unreachable is a radio that looks
// broken rather than a radio with a bad connection. This list is the floor -
// something to play that needs no query to find, and a place to start for a
// user who does not yet know what to search for.
//
// Every entry was fetched before it was written down. The bar was HTTP 200 with
// `Content-Type: audio/mpeg` and 64 KB of body actually read off the socket,
// measured 2026-09-06. Anything that answered with HLS or with a playlist was
// left out rather than shipped as a row that fails the moment it is tapped.
//
// This list is MP3-only, and that is now a property of the LIST, not of the
// app. The app gained an AAC decoder after this table was written and plays
// ADTS AAC and HE-AAC as well as MP3; the directory search reaches those (see
// codec_playable() in directory_parse.c). The table stayed MP3 because these
// are the URLs that were measured, and a preset's whole job is to be the thing
// that works with no query behind it - not because AAC could not go here.
//
// Three absences are worth stating outright, because they are the obvious
// stations to go looking for and their absence is a finding, not an oversight.
// All three were RE-MEASURED 2026-09-07, after AAC support landed, rather than
// left standing on the older note:
//
//   * BBC Radio 2 and BBC Radio 6 Music are still out, and AAC support does not
//     change that, because the blocker was never the codec. Both resolve to an
//     .m3u8: Radio 2 answers 228 bytes beginning `23 45 58 54 4D 33 55` -
//     "#EXTM3U" - with Content-Type application/x-mpegurl, and 6 Music answers a
//     592-byte EXT-X-MEDIA-SEQUENCE playlist. All three radio-browser entries
//     for 6 Music are .m3u8 with hls:1. HLS is a manifest listing segment files;
//     this app issues one GET and reads bytes until told to stop, so there is
//     nothing here it could play. The old bbcmedia.co.uk shoutcast names still
//     do not resolve. The World Service remains the one BBC service on plain
//     MP3 and so the only BBC entry here.
//
//   * Absolute Radio IS now playable, and is reachable through search rather
//     than from this table. It serves plain ADTS AAC at 127 kbps: measured
//     first bytes `FF F9 4C 80`, which is the ADTS syncword (0xF9 & 0xF6 ==
//     0xF0), with syncwords recurring every ~317-352 bytes through a 384 KB
//     capture, Content-Type audio/aac and icy-br 127. It is not hardcoded here
//     for one specific reason: the only URL that works carries Bauer's own
//     session parameters (aw_0_1st.skey and friends). Stripped back to
//     `.../absoluteradiohigh.aac` it answers 500, and `?direct=true` alone also
//     answers 500 - which is the same 500 the earlier note recorded, so that
//     observation was right, it was just not the whole picture. Baking another
//     party's session key into a public repository is both fragile and not ours
//     to ship. radio-browser lists all six Absolute Radio services as AAC or
//     AAC+ with hls:0 and keeps their url_resolved current, so the directory is
//     the correct route to them - which is the same reasoning directory.h gives
//     for preferring the directory over hardcoded URLs in the first place.
//
// Every URL is plain http on purpose, even where the station also serves https.
// Each one was checked over http specifically and delivers audio there, whether
// directly or through a redirect the stream client already follows. That saves
// a TLS handshake and the per-byte cost of the record layer on a 268 MHz ARM11,
// for a payload that is public audio with nothing to protect - the same
// reasoning directory.c gives for talking to the mirrors over http.
//
// Station names and stream URLs only. No logos, no artwork and no station
// branding of any kind ships in this app.

#include <stdbool.h>

#include "../net/directory.h"

// How many presets there are.
//
// Guaranteed not to exceed SW_STATIONS_MAX, so a caller that wants these in an
// SwStationList can build one without paging. That is a constraint on this list
// rather than a property of it: the cap is the directory's, and the presets
// stay inside it so the two are interchangeable everywhere.
int swPresetCount(void);

// Copies preset `index` into `out` as an ordinary station, so a preset can be
// handed to the player, to favourites, or to anything else that already takes
// an SwStation without knowing where it came from.
//
// The uuid comes back empty, deliberately. These are not directory records, and
// inventing one would make swDirRegisterPlay credit a station that may not be
// the station the URL actually plays. Empty is the honest value and both
// callers already handle it: the ping declines to send, and favourites falls
// back to matching on the URL.
//
// Returns false for an out-of-range index, leaving `out` untouched.
bool swPresetGet(int index, SwStation *out);

// The fields a list or a filter needs, without copying a whole SwStation to
// read one of them - drawing a screenful of rows should not cost a screenful of
// 456-byte struct copies.
//
// The pointers are into .rodata and stay valid for the life of the process.
// Out-of-range gives "" and 0 rather than NULL, so a caller that forgets to
// range-check draws an empty row instead of dereferencing nothing.
const char *swPresetName(int index);
const char *swPresetCountry(int index);
int         swPresetBitrate(int index);
