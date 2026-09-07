#pragma once

// The single source of truth for the version. The updater compares the tag it
// reads from GitHub against this string, and the release asset is named after
// it, so a release where this was not bumped is a release the console will
// download over and over.
#define SKYWAVE_VERSION "1.0.5"
