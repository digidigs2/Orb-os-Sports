#pragma once
// ALL CAPS, for any line of text a theme draws.
//
// Zion asked for it on every text block on every screen (2026-09-23), including the ones
// nobody can retype: the firmware version and the network address on the splash screen are
// written by the device, not by the designer, so a design that wants small caps across its
// face could not have them there at any price.
//
// Applied to the FINISHED string, at the moment of drawing, rather than to the format the
// designer typed. That is the only place it can work for all of them at once: "%A" becomes
// Wednesday inside strftime, "{callsign}" becomes UAL328 inside the token expander, and the
// version line is built from a compiled constant. Uppercasing the recipe would miss every
// one of those.
//
// ASCII ONLY, deliberately. The faces this device carries cover 0x20 to 0x7F (splash_lines
// says so at its own call site), so an accented letter has no uppercase form to draw and
// turning it into one would replace a character the theme can show with an empty box. Left
// alone, it stays exactly as it was.
#include <ctype.h>
#include <stdio.h>
#include <stddef.h>

inline void orb_upper(char *s) {
    if (!s) return;
    for (; *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') *s = (char)(c - 32);
    }
}

// The same thing, for the draw sites that hold a const string and a buffer they own.
inline const char *orb_upper_into(char *buf, size_t n, const char *src, bool on) {
    if (!on || !src) return src;
    snprintf(buf, n, "%s", src);
    orb_upper(buf);
    return buf;
}
