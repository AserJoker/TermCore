#include <termcore/tc_text.h>

/* --------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------ */

void tc_text_config_default(tc_text_config* cfg) {
    if (!cfg) return;
    cfg->replacement_cp = 0;   /* per-mode default: '?' (ASCII) / U+FFFD (Unicode) */
    cfg->tab_columns    = 0;   /* caller interprets */
    cfg->reserved[0]    = 0;
    cfg->reserved[1]    = 0;
}

/* --------------------------------------------------------------------------
 * UTF-8 decoding (docs/05 §4)
 *
 * Strict rules (docs/05 §4.1):
 *   1 byte  00-7F
 *   2 byte  C2-DF 80-BF                      (rejects overlong C0/C1)
 *   3 byte  E0 A0-BF / E1-EC / ED 80-9F / EE-EF     (rejects surrogate ED A0-BF)
 *   4 byte  F0 90-BF / F1-F3 / F4 80-8F              (cap U+10FFFF, rejects F5-FF)
 *
 * Contract:
 *   TC_UTF8_INCOMPLETE does NOT advance the input pointer (caller keeps the
 *   bytes and retries with more data).
 *   TC_UTF8_INVALID consumes exactly ONE byte and yields U+FFFD; the remaining
 *   bytes are re-examined on the next call (no swallowing).
 * ------------------------------------------------------------------------ */

#define TC_TEXT_REPLACEMENT 0xFFFDu  /* U+FFFD */

static bool utf8_is_cont(unsigned char b) {
    return (b & 0xC0u) == 0x80u;
}

tc_utf8_err tc_text_utf8_next(const char** s, const char* end, uint32_t* out_cp) {
    const unsigned char* p;
    const unsigned char* e;
    unsigned char b0, b1;
    uint32_t cp;

    if (!s || !end || !out_cp) return TC_UTF8_INVALID;

    p = (const unsigned char*)*s;
    e = (const unsigned char*)end;

    if (p >= e) return TC_UTF8_INCOMPLETE;   /* no bytes available */

    b0 = p[0];

    if (b0 < 0x80u) {                         /* ASCII / NUL */
        *s += 1;
        *out_cp = b0;
        return TC_UTF8_OK;
    }

    if (b0 < 0xC2u) {                         /* 0x80-0xBF lone continuation; 0xC0/0xC1 overlong head */
        *s += 1;
        *out_cp = TC_TEXT_REPLACEMENT;
        return TC_UTF8_INVALID;
    }

    if (b0 < 0xE0u) {                         /* 2 bytes: C2-DF */
        if (e - p < 2) return TC_UTF8_INCOMPLETE;
        b1 = p[1];
        if (!utf8_is_cont(b1)) { *s += 1; *out_cp = TC_TEXT_REPLACEMENT; return TC_UTF8_INVALID; }
        cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(b1 & 0x3Fu);
        *s += 2;
        *out_cp = cp;
        return TC_UTF8_OK;
    }

    if (b0 < 0xF0u) {                         /* 3 bytes: E0-EF */
        unsigned char lo = 0x80u, hi = 0xBFu;
        unsigned char b2;

        if (e - p < 3) return TC_UTF8_INCOMPLETE;
        if (b0 == 0xE0u) lo = 0xA0u;          /* reject overlong 3-byte */
        else if (b0 == 0xEDu) hi = 0x9Fu;     /* reject UTF-16 surrogates */

        b1 = p[1];
        if (b1 < lo || b1 > hi) { *s += 1; *out_cp = TC_TEXT_REPLACEMENT; return TC_UTF8_INVALID; }
        b2 = p[2];
        if (!utf8_is_cont(b2)) { *s += 1; *out_cp = TC_TEXT_REPLACEMENT; return TC_UTF8_INVALID; }

        cp = ((uint32_t)(b0 & 0x0Fu) << 12) |
             ((uint32_t)(b1 & 0x3Fu) << 6) |
             (uint32_t)(b2 & 0x3Fu);
        *s += 3;
        *out_cp = cp;
        return TC_UTF8_OK;
    }

    if (b0 < 0xF5u) {                         /* 4 bytes: F0-F4 */
        unsigned char lo = 0x80u, hi = 0xBFu;
        unsigned char b2, b3;

        if (e - p < 4) return TC_UTF8_INCOMPLETE;
        if (b0 == 0xF0u) lo = 0x90u;          /* reject overlong 4-byte */
        else if (b0 == 0xF4u) hi = 0x8Fu;     /* cap at U+10FFFF */

        b1 = p[1];
        if (b1 < lo || b1 > hi) { *s += 1; *out_cp = TC_TEXT_REPLACEMENT; return TC_UTF8_INVALID; }
        b2 = p[2];
        if (!utf8_is_cont(b2)) { *s += 1; *out_cp = TC_TEXT_REPLACEMENT; return TC_UTF8_INVALID; }
        b3 = p[3];
        if (!utf8_is_cont(b3)) { *s += 1; *out_cp = TC_TEXT_REPLACEMENT; return TC_UTF8_INVALID; }

        cp = ((uint32_t)(b0 & 0x07u) << 18) |
             ((uint32_t)(b1 & 0x3Fu) << 12) |
             ((uint32_t)(b2 & 0x3Fu) << 6) |
             (uint32_t)(b3 & 0x3Fu);
        *s += 4;
        *out_cp = cp;
        return TC_UTF8_OK;
    }

    /* 0xF5-0xFF: out of range head byte */
    *s += 1;
    *out_cp = TC_TEXT_REPLACEMENT;
    return TC_UTF8_INVALID;
}

tc_utf8_err tc_text_utf8_validate(const char* s, const char* end, size_t* bad_offset) {
    const char* p = s;
    uint32_t cp;

    if (!s || !end) return TC_UTF8_INVALID;

    while (p < end) {
        const char* start = p;
        tc_utf8_err r = tc_text_utf8_next(&p, end, &cp);
        if (r == TC_UTF8_INVALID || r == TC_UTF8_INCOMPLETE) {
            /* INCOMPLETE here means the string ends mid-sequence: the byte at
             * `start` started a sequence that never completed, so the whole
             * range is not valid. INVALID already consumed a byte; both point
             * bad_offset at the offending (first) byte. */
            if (bad_offset) *bad_offset = (size_t)(start - s);
            return TC_UTF8_INVALID;
        }
    }
    return TC_UTF8_OK;
}

size_t tc_text_utf8_encode(uint32_t cp, char* out4) {
    unsigned char* o = (unsigned char*)out4;

    if (!out4) return 0;

    if (cp < 0x80u) {
        o[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        o[0] = (unsigned char)(0xC0u | (cp >> 6));
        o[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;   /* surrogates not encodable */
        o[0] = (unsigned char)(0xE0u | (cp >> 12));
        o[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        o[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        o[0] = (unsigned char)(0xF0u | (cp >> 18));
        o[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
        o[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        o[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;   /* beyond U+10FFFF */
}

/* --------------------------------------------------------------------------
 * Grapheme clusters, width, measure/layout helpers
 *
 * These land with the next interface milestones (docs/05 §5/§6/§7). When the
 * ICU backend is enabled they are backed by ICU (ubrk / u_getIntPropertyValue)
 * behind the same API; the default build carries the generated UCD tables.
 * ------------------------------------------------------------------------ */
