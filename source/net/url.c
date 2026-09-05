#include "url.h"

#include <stdio.h>
#include <string.h>

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool swUrlToPlainHttp(const char *in, char *out, size_t cap)
{
    static const char kHttps[] = "https://";
    static const char kHttp[]  = "http://";
    const size_t https_len = sizeof(kHttps) - 1;   // 8
    const size_t http_len  = sizeof(kHttp) - 1;    // 7

    if (!in || !out || cap == 0) return false;

    // Stops at the terminator on its own: NUL matches nothing in kHttps, so a
    // string shorter than the scheme fails before reading past its end.
    for (size_t i = 0; i < https_len; i++) {
        if (lower(in[i]) != kHttps[i]) return false;
    }

    const char *rest = in + https_len;

    // Checked before writing, so a URL that does not fit leaves `out` alone
    // rather than handing back a truncated address that would connect to the
    // wrong place - a silently wrong stream is worse than a refusal.
    if (strlen(rest) + http_len + 1 > cap) return false;

    snprintf(out, cap, "%s%s", kHttp, rest);
    return true;
}
