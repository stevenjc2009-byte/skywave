#pragma once

// SKYWAVE-AUTHORED COMPATIBILITY SHIM - see Arduino.h in this same directory
// for why this exists. aaccommon.h does `#include <pgmspace.h>` right after
// `#include <Arduino.h>`; every macro it would define is already defined by
// our Arduino.h shim, so this file is intentionally empty.
