#include <termcore/tc_text.h>

#ifdef TERMCORE_USE_ICU
#include <unicode/uchar.h>    /* u_charType, u_getIntPropertyValue */
#include <unicode/ubrk.h>     /* UBreakIterator, UBRK_CHARACTER */
#include <unicode/umachine.h>
#include <unicode/utypes.h>
#endif

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
 * Shared helpers
 *
 * The grapheme/width/layout APIs (docs/05 §5/§6/§7) are backed by ICU
 * (ubrk / u_getIntPropertyValue) when TERMCORE_USE_ICU is enabled; otherwise
 * they degrade to a conservative per-codepoint split with built-in CJK width
 * ranges. The public tc_text.h stays dependency-free either way.
 * ------------------------------------------------------------------------ */

#define TC_GRAPHEME_MAX_CP 32   /* cluster size safety cap (docs/05 §5.1) */

/* Decode exactly one codepoint, settling a trailing incomplete sequence as
 * U+FFFD (docs/05 §4.2: at stream end, an incomplete sequence is settled as
 * one U+FFFD consuming the whole dangling tail). Returns the consumed byte
 * count and sets *out_cp. */
static size_t tc_decode_one(const char* s, const char* end, uint32_t* out_cp) {
    const char* p = s;
    tc_utf8_err r = tc_text_utf8_next(&p, end, out_cp);
    if (r == TC_UTF8_INCOMPLETE) {
        p = end;                     /* settle the whole dangling tail as one U+FFFD */
        *out_cp = TC_TEXT_REPLACEMENT;
    } else if (r == TC_UTF8_INVALID) {
        *out_cp = TC_TEXT_REPLACEMENT;   /* already consumed one byte */
    }
    return (size_t)(p - s);
}

/* --------------------------------------------------------------------------
 * Grapheme clusters (docs/05 §5)
 * ------------------------------------------------------------------------ */

bool tc_text_grapheme_next(const char** s, const char* end, tc_grapheme* out) {
    const char* p;
    uint32_t first_cp = 0;
    bool have_first = false;

    if (!s || !end || !out) return false;
    p = *s;
    if (p >= end) return false;

#ifdef TERMCORE_USE_ICU
    {
        enum { MAX_CP = TC_GRAPHEME_MAX_CP + 1 };   /* one extra to decide the boundary */
        UChar ubuf[MAX_CP * 2];
        int32_t cp_byte_off[MAX_CP + 1];   /* byte offset of each cp start, plus total */
        int32_t cp_u16[MAX_CP];            /* UTF-16 units per codepoint */
        int32_t ncp = 0, nchar = 0;

        cp_byte_off[0] = 0;
        while (p < end && ncp < MAX_CP) {
            uint32_t cp;
            p += tc_decode_one(p, end, &cp);
            if (!have_first) { first_cp = cp; have_first = true; }
            if (cp < 0x10000u) {
                ubuf[nchar++] = (UChar)cp;
                cp_u16[ncp] = 1;
            } else {                            /* surrogate pair */
                ubuf[nchar++] = (UChar)(0xD7C0u + (cp >> 10));
                ubuf[nchar++] = (UChar)(0xDC00u + (cp & 0x3FFu));
                cp_u16[ncp] = 2;
            }
            ncp++;
            cp_byte_off[ncp] = (int32_t)(p - *s);
        }

        {
            UErrorCode st = U_ZERO_ERROR;
            UBreakIterator* bi = ubrk_open(UBRK_CHARACTER, "", NULL, 0, &st);
            if (U_SUCCESS(st) && bi) {
                int32_t b1 = -1;
                ubrk_setText(bi, ubuf, nchar, &st);
                if (U_SUCCESS(st)) b1 = ubrk_next(bi);
                ubrk_close(bi);
                if (U_SUCCESS(st) && b1 != UBRK_DONE) {
                    int32_t u16_total = 0, ci = 0;
                    while (ci < ncp && u16_total < b1) { u16_total += cp_u16[ci]; ci++; }
                    if (ci > TC_GRAPHEME_MAX_CP) ci = TC_GRAPHEME_MAX_CP;
                    out->begin = *s;
                    out->end = *s + cp_byte_off[ci];
                    out->first_cp = first_cp;
                    out->width = 0;   /* computed by tc_text_grapheme_width */
                    out->reserved[0] = 0;
                    out->reserved[1] = 0;
                    *s = out->end;
                    return true;
                }
            }
        }
        /* ICU unavailable at runtime: fall through to per-codepoint split */
    }
#endif /* TERMCORE_USE_ICU */

    {
        const char* const start = *s;
        uint32_t cp;
        *s += tc_decode_one(*s, end, &cp);
        out->begin = start;
        out->end = *s;
        out->first_cp = cp;
        out->width = 0;
        out->reserved[0] = 0;
        out->reserved[1] = 0;
        return true;
    }
}

bool tc_text_grapheme_next_off(const char* s, size_t len, size_t* inout_offset, tc_grapheme* out) {
    const char* p;
    const char* end;

    if (!s || !inout_offset || !out) return false;
    if (*inout_offset >= len) return false;

    p = s + *inout_offset;
    end = s + len;
    if (!tc_text_grapheme_next(&p, end, out)) return false;
    *inout_offset = (size_t)(p - s);
    return true;
}

/* --------------------------------------------------------------------------
 * Width (docs/05 §6)
 * ------------------------------------------------------------------------ */

/* Degraded-path wide ranges (CJK / fullwidth / emoji). Only used when the ICU
 * backend is disabled. */
static bool tc_degraded_is_wide(uint32_t cp) {
    return (cp >= 0x1100u && cp <= 0x115Fu) ||   /* Hangul Jamo */
           (cp >= 0x2E80u && cp <= 0x303Eu) ||   /* CJK Radicals..Symbols */
           (cp >= 0x3041u && cp <= 0x33FFu) ||   /* Hiragana..CJK Compat */
           (cp >= 0x3400u && cp <= 0x4DBFu) ||   /* CJK Ext A */
           (cp >= 0x4E00u && cp <= 0x9FFFu) ||   /* CJK Unified */
           (cp >= 0xA000u && cp <= 0xA4CFu) ||   /* Yi */
           (cp >= 0xAC00u && cp <= 0xD7A3u) ||   /* Hangul syllables */
           (cp >= 0xF900u && cp <= 0xFAFFu) ||   /* CJK Compat Ideographs */
           (cp >= 0xFE30u && cp <= 0xFE4Fu) ||   /* CJK Compat Forms */
           (cp >= 0xFF00u && cp <= 0xFF60u) ||   /* Fullwidth Forms */
           (cp >= 0xFFE0u && cp <= 0xFFE6u) ||
           (cp >= 0x1F300u && cp <= 0x1FAFFu);   /* emoji */
}

/* Degraded-path combining-mark ranges (Mn/Me/Mc, common blocks). The ICU
 * backend classifies these via u_charType; without ICU we keep a compact table
 * so combining marks still take zero cells (docs/05 §6.1 rule 6). */
static bool tc_degraded_is_combining(uint32_t cp) {
    return (cp >= 0x0300u && cp <= 0x036Fu) ||   /* Combining Diacritical Marks */
           (cp >= 0x0483u && cp <= 0x0489u) ||
           (cp >= 0x0591u && cp <= 0x05BDu) ||
           (cp == 0x05BFu) ||
           (cp >= 0x05C1u && cp <= 0x05C2u) ||
           (cp >= 0x05C4u && cp <= 0x05C5u) ||
           (cp == 0x05C7u) ||
           (cp >= 0x0610u && cp <= 0x061Au) ||
           (cp >= 0x064Bu && cp <= 0x065Fu) ||
           (cp == 0x0670u) ||
           (cp >= 0x06D6u && cp <= 0x06DCu) ||
           (cp >= 0x06DFu && cp <= 0x06E4u) ||
           (cp >= 0x06E7u && cp <= 0x06E8u) ||
           (cp >= 0x06EAu && cp <= 0x06EDu) ||
           (cp == 0x0711u) ||
           (cp >= 0x0730u && cp <= 0x074Au) ||
           (cp >= 0x07A6u && cp <= 0x07B0u) ||
           (cp >= 0x07EBu && cp <= 0x07F3u) ||
           (cp >= 0x0816u && cp <= 0x0819u) ||
           (cp >= 0x081Bu && cp <= 0x0823u) ||
           (cp >= 0x0825u && cp <= 0x0827u) ||
           (cp >= 0x0829u && cp <= 0x082Du) ||
           (cp >= 0x0859u && cp <= 0x085Bu) ||
           (cp >= 0x08D3u && cp <= 0x0903u) ||
           (cp >= 0x093Au && cp <= 0x093Cu) ||
           (cp >= 0x093Eu && cp <= 0x094Fu) ||
           (cp >= 0x0951u && cp <= 0x0957u) ||
           (cp >= 0x0962u && cp <= 0x0963u) ||
           (cp >= 0x09BCu && cp <= 0x09BCu) ||
           (cp >= 0x09BEu && cp <= 0x09C4u) ||
           (cp >= 0x09C7u && cp <= 0x09C8u) ||
           (cp >= 0x09CBu && cp <= 0x09CDu) ||
           (cp == 0x09D7u) ||
           (cp >= 0x09E2u && cp <= 0x09E3u) ||
           (cp == 0x0A3Cu) ||
           (cp >= 0x0A3Eu && cp <= 0x0A42u) ||
           (cp >= 0x0A47u && cp <= 0x0A48u) ||
           (cp >= 0x0A4Bu && cp <= 0x0A4Du) ||
           (cp == 0x0A51u) ||
           (cp >= 0x0A70u && cp <= 0x0A71u) ||
           (cp == 0x0A75u) ||
           (cp >= 0x1AB0u && cp <= 0x1AFFu) ||   /* Combining Marks Extended */
           (cp >= 0x1DC0u && cp <= 0x1DFFu) ||   /* Combining Marks Supplement */
           (cp >= 0x20D0u && cp <= 0x20FFu) ||   /* Combining Marks for Symbols */
           (cp >= 0xFE00u && cp <= 0xFE0Fu) ||   /* Variation Selectors (incl. VS15/16) */
           (cp >= 0xFE20u && cp <= 0xFE2Fu);     /* Combining Half Marks */
}

/* Degraded-path East Asian Ambiguous ranges (UCD EastAsianWidth.txt, class A),
 * sampled to common blocks. `ambiguous_wide` then decides 1 vs 2 columns
 * (docs/05 §6.2). */
static bool tc_degraded_is_ambiguous(uint32_t cp) {
    return (cp >= 0x00A1u && cp <= 0x00A1u) ||
           (cp == 0x00A4u) ||
           (cp >= 0x00A7u && cp <= 0x00A8u) ||
           (cp == 0x00AAu) ||
           (cp >= 0x00ADu && cp <= 0x00AEu) ||
           (cp >= 0x00B0u && cp <= 0x00B4u) ||
           (cp >= 0x00B6u && cp <= 0x00BAu) ||
           (cp >= 0x00BCu && cp <= 0x00BFu) ||
           (cp == 0x00C6u) ||
           (cp == 0x00D0u) ||
           (cp >= 0x00D7u && cp <= 0x00D8u) ||
           (cp >= 0x00DEu && cp <= 0x00E1u) ||
           (cp == 0x00E6u) ||
           (cp >= 0x00E8u && cp <= 0x00EAu) ||
           (cp >= 0x00ECu && cp <= 0x00EDu) ||
           (cp == 0x00F0u) ||
           (cp >= 0x00F2u && cp <= 0x00F3u) ||
           (cp >= 0x00F7u && cp <= 0x00FAu) ||
           (cp == 0x00FCu) ||
           (cp == 0x00FEu) ||
           (cp == 0x0101u) ||
           (cp == 0x0111u) ||
           (cp == 0x0113u) ||
           (cp == 0x011Bu) ||
           (cp >= 0x0126u && cp <= 0x0127u) ||
           (cp == 0x012Bu) ||
           (cp >= 0x0131u && cp <= 0x0133u) ||
           (cp == 0x0138u) ||
           (cp >= 0x013Fu && cp <= 0x0142u) ||
           (cp == 0x0144u) ||
           (cp >= 0x0148u && cp <= 0x014Bu) ||
           (cp == 0x014Du) ||
           (cp >= 0x0152u && cp <= 0x0153u) ||
           (cp >= 0x0166u && cp <= 0x0167u) ||
           (cp == 0x016Bu) ||
           (cp >= 0x01CEu && cp <= 0x01CEu) ||
           (cp == 0x01D0u) ||
           (cp == 0x01D2u) ||
           (cp == 0x01D4u) ||
           (cp == 0x01D6u) ||
           (cp == 0x01D8u) ||
           (cp == 0x01DAu) ||
           (cp == 0x01DCu) ||
           (cp == 0x0251u) ||
           (cp == 0x0261u) ||
           (cp == 0x02C4u) ||
           (cp == 0x02C7u) ||
           (cp >= 0x02C9u && cp <= 0x02CBu) ||
           (cp == 0x02CDu) ||
           (cp == 0x02D0u) ||
           (cp >= 0x02D8u && cp <= 0x02DBu) ||
           (cp == 0x02DDu) ||
           (cp == 0x02DFu) ||
           (cp >= 0x2010u && cp <= 0x2010u) ||   /* ‐ */
           (cp >= 0x2013u && cp <= 0x2016u) ||
           (cp >= 0x2018u && cp <= 0x2019u) ||
           (cp >= 0x201Cu && cp <= 0x201Du) ||
           (cp >= 0x2020u && cp <= 0x2022u) ||
           (cp >= 0x2024u && cp <= 0x2027u) ||
           (cp == 0x2030u) ||
           (cp >= 0x2032u && cp <= 0x2033u) ||
           (cp == 0x2035u) ||
           (cp == 0x203Bu) ||
           (cp == 0x203Eu) ||
           (cp == 0x2074u) ||
           (cp == 0x207Fu) ||
           (cp >= 0x2081u && cp <= 0x2084u) ||
           (cp == 0x20ACu) ||
           (cp == 0x2103u) ||
           (cp == 0x2105u) ||
           (cp == 0x2109u) ||
           (cp == 0x2113u) ||
           (cp == 0x2116u) ||
           (cp >= 0x2121u && cp <= 0x2122u) ||
           (cp == 0x2126u) ||
           (cp == 0x212Bu) ||
           (cp == 0x2153u) ||
           (cp >= 0x215Bu && cp <= 0x215Eu) ||
           (cp >= 0x2160u && cp <= 0x216Bu) ||
           (cp >= 0x2170u && cp <= 0x2179u) ||
           (cp >= 0x2189u && cp <= 0x2189u) ||
           (cp >= 0x2190u && cp <= 0x2199u) ||
           (cp >= 0x21B8u && cp <= 0x21B9u) ||
           (cp == 0x21D2u) ||
           (cp == 0x21D4u) ||
           (cp == 0x21E7u) ||
           (cp == 0x2200u) ||
           (cp >= 0x2202u && cp <= 0x2203u) ||
           (cp >= 0x2207u && cp <= 0x2208u) ||
           (cp == 0x220Bu) ||
           (cp == 0x220Fu) ||
           (cp == 0x2211u) ||
           (cp == 0x2215u) ||
           (cp == 0x221Au) ||
           (cp >= 0x221Du && cp <= 0x2220u) ||
           (cp == 0x2223u) ||
           (cp == 0x2225u) ||
           (cp >= 0x2227u && cp <= 0x222Cu) ||
           (cp == 0x222Eu) ||
           (cp >= 0x2234u && cp <= 0x2237u) ||
           (cp == 0x223Cu) ||
           (cp == 0x223Du) ||
           (cp == 0x2248u) ||
           (cp == 0x224Cu) ||
           (cp == 0x2252u) ||
           (cp == 0x2260u) ||
           (cp == 0x2261u) ||
           (cp >= 0x2264u && cp <= 0x2267u) ||
           (cp == 0x226Au) ||
           (cp == 0x226Bu) ||
           (cp == 0x226Eu) ||
           (cp == 0x226Fu) ||
           (cp == 0x2282u) ||
           (cp == 0x2283u) ||
           (cp == 0x2286u) ||
           (cp == 0x2287u) ||
           (cp == 0x2295u) ||
           (cp == 0x2299u) ||
           (cp == 0x22A5u) ||
           (cp == 0x22BFu) ||
           (cp == 0x2312u) ||
           (cp >= 0x2500u && cp <= 0x254Bu) ||   /* Box Drawing */
           (cp >= 0x2550u && cp <= 0x2573u) ||
           (cp >= 0x2580u && cp <= 0x258Fu) ||   /* Block Elements */
           (cp >= 0x2592u && cp <= 0x2595u) ||
           (cp >= 0x25A0u && cp <= 0x25A1u) ||   /* Geometric Shapes */
           (cp >= 0x25A3u && cp <= 0x25A9u) ||
           (cp >= 0x25B2u && cp <= 0x25B3u) ||
           (cp >= 0x25B6u && cp <= 0x25B7u) ||
           (cp >= 0x25BCu && cp <= 0x25BDu) ||
           (cp >= 0x25C0u && cp <= 0x25C1u) ||
           (cp >= 0x25C6u && cp <= 0x25C8u) ||
           (cp == 0x25CBu) ||
           (cp >= 0x25CEu && cp <= 0x25D1u) ||
           (cp >= 0x25E2u && cp <= 0x25E5u) ||
           (cp == 0x25EFu) ||
           (cp >= 0x2605u && cp <= 0x2606u) ||   /* Misc Symbols */
           (cp == 0x2609u) ||
           (cp >= 0x260Eu && cp <= 0x260Fu) ||
           (cp == 0x261Cu) ||
           (cp == 0x261Eu) ||
           (cp == 0x2640u) ||
           (cp == 0x2642u) ||
           (cp >= 0x2660u && cp <= 0x2661u) ||   /* Card Suits */
           (cp >= 0x2663u && cp <= 0x2665u) ||
           (cp >= 0x2667u && cp <= 0x266Au) ||
           (cp == 0x266Cu) ||
           (cp == 0x266Du) ||
           (cp == 0x266Fu) ||
           (cp == 0x269Eu) ||
           (cp >= 0x26BDu && cp <= 0x26BEu) ||
           (cp == 0x26BFu) ||
           (cp >= 0x26C0u && cp <= 0x26C3u) ||
           (cp >= 0x26CDu) ||
           (cp == 0x26CFu) ||
           (cp == 0x26D3u) ||
           (cp == 0x26D4u) ||
           (cp >= 0x26D5u && cp <= 0x26E1u) ||
           (cp == 0x26E3u) ||
           (cp == 0x26E8u) ||
           (cp == 0x26E9u) ||
           (cp >= 0x26EBu && cp <= 0x26F1u) ||
           (cp == 0x26F4u) ||
           (cp >= 0x26F6u && cp <= 0x26F9u) ||
           (cp == 0x26FBu) ||
           (cp == 0x26FCu) ||
           (cp == 0x26FEu) ||
           (cp == 0x26FFu) ||
           (cp == 0x273Du) ||
           (cp == 0x2757u) ||
           (cp == 0x2776u) ||
           (cp >= 0x2B55u && cp <= 0x2B59u) ||
           (cp >= 0x1F100u && cp <= 0x1F10Cu) || /* Enclosed Alphanum Supplement (A) */
           (cp >= 0x1F110u && cp <= 0x1F16Cu) ||
           (cp >= 0x1F170u && cp <= 0x1F1ACu) ||
           (cp >= 0x1F200u && cp <= 0x1F202u) ||
           (cp >= 0x1F210u && cp <= 0x1F23Bu) ||
           (cp >= 0x1F240u && cp <= 0x1F248u) ||
           (cp >= 0x1F250u && cp <= 0x1F251u);
}

int32_t tc_text_char_width(uint32_t cp, tc_text_mode mode, bool ambiguous_wide) {
    if (mode == TC_TEXT_ASCII) return 1;

    /* Control characters / NUL take no cell (docs/05 §6.1 rule 6). */
    if (cp == 0u || cp < 0x20u || cp == 0x7Fu || (cp >= 0x80u && cp < 0xA0u)) return 0;

#ifdef TERMCORE_USE_ICU
    {
        int8_t cat = u_charType((UChar32)cp);
        if (cat == U_NON_SPACING_MARK || cat == U_ENCLOSING_MARK ||
            cat == U_COMBINING_SPACING_MARK) {
            return 0;   /* combining marks take no cell */
        }
        int32_t eaw = u_getIntPropertyValue((UChar32)cp, UCHAR_EAST_ASIAN_WIDTH);
        if (eaw == U_EA_WIDE || eaw == U_EA_FULLWIDTH) return 2;
        if (eaw == U_EA_AMBIGUOUS) return ambiguous_wide ? 2 : 1;
        return 1;
    }
#else
    if (tc_degraded_is_combining(cp)) return 0;   /* combining marks take no cell */
    if (tc_degraded_is_wide(cp)) return 2;
    if (tc_degraded_is_ambiguous(cp)) return ambiguous_wide ? 2 : 1;
    return 1;
#endif
}

int32_t tc_text_grapheme_width(const tc_grapheme* g, tc_text_mode mode, bool ambiguous_wide) {
    const char* p;
    const char* end;
    uint32_t base = 0;
    uint32_t first_non_comb = 0;
    bool has_base = false;
    bool has_vs16 = false, has_vs15 = false, has_zwj = false, any_non_comb = false;
    int32_t ri_count = 0;

    if (!g || g->begin >= g->end) return 0;
    if (mode == TC_TEXT_ASCII) return 1;

    p = g->begin;
    end = g->end;
    while (p < end) {
        uint32_t cp;
        p += tc_decode_one(p, end, &cp);
        if (!has_base) { base = cp; has_base = true; }

        if (cp == 0xFE0Fu) has_vs16 = true;          /* VS16: emoji presentation */
        else if (cp == 0xFE0Eu) has_vs15 = true;     /* VS15: text presentation */
        else if (cp == 0x200Du) has_zwj = true;

        if (cp >= 0x1F1E6u && cp <= 0x1F1FFu) ri_count++;   /* Regional_Indicator */

#ifdef TERMCORE_USE_ICU
        {
            int8_t cat = u_charType((UChar32)cp);
            bool comb = (cat == U_NON_SPACING_MARK || cat == U_ENCLOSING_MARK ||
                         cat == U_COMBINING_SPACING_MARK || cp == 0x200Du /* ZWJ */);
            if (!comb) {
                if (!first_non_comb) first_non_comb = cp;
                any_non_comb = true;
            }
        }
#else
        if (!tc_degraded_is_combining(cp) && cp != 0x200Du) {
            if (!first_non_comb) first_non_comb = cp;
            any_non_comb = true;
        }
#endif
    }

    /* docs/05 §6.1 judgement order. */
#ifdef TERMCORE_USE_ICU
    if (has_vs16 && has_base &&
        (u_getIntPropertyValue((UChar32)base, UCHAR_EMOJI_PRESENTATION) ||
         u_getIntPropertyValue((UChar32)base, UCHAR_EXTENDED_PICTOGRAPHIC))) {
        return 2;                                    /* rule 1: VS16 + emoji base */
    }
#endif
    if (has_vs15 && first_non_comb) {
        return tc_text_char_width(first_non_comb, mode, ambiguous_wide);   /* rule 2 */
    }
#ifdef TERMCORE_USE_ICU
    if (has_zwj && has_base &&
        u_getIntPropertyValue((UChar32)base, UCHAR_EXTENDED_PICTOGRAPHIC)) {
        return 2;                                    /* rule 3: emoji ZWJ sequence */
    }
#endif
    if (ri_count >= 2) return 2;                     /* rule 4: paired RI (flag) */
    if (!any_non_comb) return 0;                     /* rule 6: pure combining cluster */
    if (first_non_comb) return tc_text_char_width(first_non_comb, mode, ambiguous_wide);  /* rule 5 */
    return 0;
}

int32_t tc_text_string_width(const char* utf8, size_t len, tc_text_mode mode, bool ambiguous_wide) {
    const char* p;
    const char* end;
    int32_t total;

    if (!utf8 || len == 0) return 0;
    if (mode == TC_TEXT_ASCII) return (int32_t)len;

    total = 0;
    p = utf8;
    end = utf8 + len;
    {
        tc_grapheme g;
        while (tc_text_grapheme_next(&p, end, &g)) {
            total += tc_text_grapheme_width(&g, mode, ambiguous_wide);
        }
    }
    return total;
}

/* --------------------------------------------------------------------------
 * Measure and layout helpers (docs/05 §7)
 * ------------------------------------------------------------------------ */

tc_text_metrics tc_text_measure(const char* utf8, size_t len,
                                tc_text_mode mode, bool ambiguous_wide) {
    tc_text_metrics m;
    const char* p;
    const char* end;

    m.columns = 0;
    m.graphemes = 0;
    m.codepoints = 0;
    m.bytes = 0;
    if (!utf8 || len == 0) return m;

    if (mode == TC_TEXT_ASCII) {
        m.columns = (int32_t)len;
        m.graphemes = (int32_t)len;
        m.codepoints = (int32_t)len;
        m.bytes = (int32_t)len;
        return m;
    }

    p = utf8;
    end = utf8 + len;
    {
        tc_grapheme g;
        while (tc_text_grapheme_next(&p, end, &g)) {
            const char* q;
            m.columns += tc_text_grapheme_width(&g, mode, ambiguous_wide);
            m.graphemes++;
            m.bytes = (int32_t)(g.end - utf8);
            q = g.begin;
            while (q < g.end) {
                uint32_t cp;
                q += tc_decode_one(q, g.end, &cp);
                m.codepoints++;
            }
        }
    }
    return m;
}

bool tc_text_truncate_columns(const char* utf8, size_t len, int32_t max_columns,
                              tc_text_mode mode, bool ambiguous_wide,
                              size_t* out_bytes, int32_t* out_columns, bool* truncated) {
    const char* p;
    const char* end;
    size_t cur_bytes;
    int32_t cur_cols;
    bool did_truncate;

    if (!utf8) return false;
    if (max_columns < 0) max_columns = 0;

    if (mode == TC_TEXT_ASCII) {
        size_t n = len < (size_t)max_columns ? len : (size_t)max_columns;
        if (out_bytes) *out_bytes = n;
        if (out_columns) *out_columns = (int32_t)n;
        if (truncated) *truncated = (n < len);
        return true;
    }

    p = utf8;
    end = utf8 + len;
    cur_bytes = 0;
    cur_cols = 0;
    did_truncate = false;
    {
        tc_grapheme g;
        while (tc_text_grapheme_next(&p, end, &g)) {
            int32_t w = tc_text_grapheme_width(&g, mode, ambiguous_wide);
            if (cur_cols + w > max_columns) {
                did_truncate = true;
                break;   /* the whole cluster does not fit (docs/05 §7.2 rule 1) */
            }
            cur_cols += w;
            cur_bytes = (size_t)(g.end - utf8);
        }
    }
    if (out_bytes) *out_bytes = cur_bytes;
    if (out_columns) *out_columns = cur_cols;
    if (truncated) *truncated = did_truncate;
    return true;
}

/* Line-start forbidden punctuation (docs/05 §7.3): closing brackets and
 * CJK punctuation that must not open a line. Practical, not exhaustive. */
static bool tc_is_forbidden_line_start(uint32_t cp) {
    switch (cp) {
        case 0x0029u: case 0x005Du: case 0x007Du:                 /* ) ] } */
        case 0x3001u: case 0x3002u:                               /* 、 。 */
        case 0x3009u: case 0x300Bu: case 0x300Du: case 0x300Fu:   /* 〉》」』 */
        case 0x3011u: case 0x3015u:                               /* 】〕 */
        case 0xFF01u: case 0xFF09u: case 0xFF0Cu: case 0xFF0Eu:   /* ！），。 */
        case 0xFF1Au: case 0xFF1Bu: case 0xFF1Du:                 /* ：；＝ */
            return true;
        default:
            return false;
    }
}

bool tc_text_wrap_next(const char* utf8, size_t len, int32_t max_columns,
                       tc_text_mode mode, bool ambiguous_wide,
                       size_t* out_break_bytes) {
    const char* p;
    const char* end;
    int32_t cur;
    size_t last_break;
    bool have_break;
    bool first_cluster;
    tc_grapheme g;

    if (!utf8 || !out_break_bytes) return false;
    if (len == 0) return false;
    if (max_columns < 1) max_columns = 1;

    if (mode == TC_TEXT_ASCII) {
        size_t cur_bytes = 0;
        size_t lb = 0;
        bool have = false;
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)utf8[i];
            if (cur_bytes + 1 > (size_t)max_columns && cur_bytes > 0) {
                if (have) { *out_break_bytes = lb; return true; }
                *out_break_bytes = i;
                return true;
            }
            cur_bytes++;
            if (c == ' ' || c == '\t' || c == '-') { lb = i + 1; have = true; }
        }
        *out_break_bytes = len;
        return false;
    }

    p = utf8;
    end = utf8 + len;
    cur = 0;
    last_break = 0;
    have_break = false;
    first_cluster = true;

    while (tc_text_grapheme_next(&p, end, &g)) {
        int32_t w = tc_text_grapheme_width(&g, mode, ambiguous_wide);
        if (cur + w > max_columns && !first_cluster) {
            if (have_break && !tc_is_forbidden_line_start(g.first_cp)) {
                *out_break_bytes = last_break;
                return true;
            }
            if (have_break && last_break > 0) {
                *out_break_bytes = last_break;
                return true;
            }
            /* no usable opportunity: hard break before this cluster (progress) */
            *out_break_bytes = (size_t)(g.begin - utf8);
            return true;
        }
        cur += w;
        first_cluster = false;

        /* Break opportunities (docs/05 §7.3): after space/TAB/hyphen, between
         * CJK characters, after ZWSP. */
        if (g.first_cp == 0x0020u || g.first_cp == 0x0009u ||
            g.first_cp == 0x002Du || g.first_cp == 0x200Bu ||
            tc_degraded_is_wide(g.first_cp)) {
            last_break = (size_t)(g.end - utf8);
            have_break = true;
        }
    }

    /* everything fits in one line */
    *out_break_bytes = len;
    return false;
}
