#ifndef TERMCORE_TC_TEXT_H
#define TERMCORE_TC_TEXT_H

/* Zero-dependency text engine (docs/05).
 *
 * This header is intentionally self-contained: it depends only on the C
 * standard headers <stddef.h>, <stdint.h> and <stdbool.h> and pulls in no
 * other termcore header, so layout / measurement code can use it standalone.
 *
 * Every function here is a pure function: no allocation, no global state,
 * no locale probing, no environment reads. Callers that need to carry state
 * across calls (a partial UTF-8 sequence, a grapheme iteration) hold that
 * state themselves; the library never hides it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Text mode
 * ------------------------------------------------------------------------ */

typedef enum tc_text_mode {
    TC_TEXT_ASCII   = 0,   /* strict 7-bit: one byte = one cell, width 1 */
    TC_TEXT_UNICODE = 1    /* UAX#29 grapheme clusters + EAW widths */
} tc_text_mode;

/* --------------------------------------------------------------------------
 * UTF-8 decoding
 * ------------------------------------------------------------------------ */

typedef enum tc_utf8_err {
    TC_UTF8_OK         = 0,   /* one codepoint decoded */
    TC_UTF8_INCOMPLETE = 1,   /* need more bytes; input pointer NOT advanced */
    TC_UTF8_INVALID    = 2    /* invalid sequence; U+FFFD substitution applied */
} tc_utf8_err;

/* --------------------------------------------------------------------------
 * Graphemes and metrics
 * ------------------------------------------------------------------------ */

typedef struct tc_grapheme {
    const char* begin;      /* borrowed pointer into the input string */
    const char* end;        /* exclusive */
    uint32_t    first_cp;   /* first codepoint of the cluster */
    int32_t     width;      /* 0 / 1 / 2 columns */
    uint32_t    reserved[2];
} tc_grapheme;

typedef struct tc_text_metrics {
    int32_t columns;     /* total display columns */
    int32_t graphemes;   /* number of grapheme clusters */
    int32_t codepoints;  /* number of codepoints */
    int32_t bytes;       /* bytes consumed */
} tc_text_metrics;

/* --------------------------------------------------------------------------
 * Configuration
 *
 * The text engine is stateless and pure; tc_text_config only carries the few
 * knobs that are cheaper to set once than to thread through every call.
 * ------------------------------------------------------------------------ */

typedef struct tc_text_config {
    /* replacement codepoint used for unrepresentable input:
     *   0           -> per-mode default ('?' in ASCII mode, U+FFFD in Unicode)
     *   otherwise   -> explicit replacement codepoint
     * Default (from tc_text_config_default): 0. */
    uint32_t replacement_cp;
    /* TAB advance in columns; 0 means "caller interprets" (engine reports a
     * zero-width cluster and the render layer decides). Default: 0. */
    int32_t  tab_columns;
    uint32_t reserved[2];
} tc_text_config;

/* Fills *cfg with defaults. Safe on a zero-initialized struct; NULL is a
 * no-op. */
void tc_text_config_default(tc_text_config* cfg);

/* --------------------------------------------------------------------------
 * UTF-8 decoding (docs/05 §4)
 * ------------------------------------------------------------------------ */

/* Decodes one codepoint at **s (bounded by end). On success advances *s and
 * sets *out_cp. TC_UTF8_INCOMPLETE does not advance *s. TC_UTF8_INVALID
 * consumes one byte, emits U+FFFD into *out_cp and advances *s. */
tc_utf8_err tc_text_utf8_next(const char** s, const char* end, uint32_t* out_cp);

/* Validates the whole range; on failure sets *bad_offset to the byte offset
 * of the first invalid byte and returns TC_UTF8_INVALID. */
tc_utf8_err tc_text_utf8_validate(const char* s, const char* end, size_t* bad_offset);

/* Encodes cp into out4 (capacity 4). Returns the byte count written, or 0 if
 * cp is not encodable. */
size_t tc_text_utf8_encode(uint32_t cp, char* out4);

/* --------------------------------------------------------------------------
 * Grapheme clusters (docs/05 §5, full UAX#29)
 * ------------------------------------------------------------------------ */

/* Splits the next cluster off *s (bounded by end); advances *s. Returns false
 * when the string is exhausted. */
bool tc_text_grapheme_next(const char** s, const char* end, tc_grapheme* out);

/* Offset-based variant: *inout_offset is consumed by the returned cluster. */
bool tc_text_grapheme_next_off(const char* s, size_t len, size_t* inout_offset, tc_grapheme* out);

/* --------------------------------------------------------------------------
 * Width (docs/05 §6)
 * ------------------------------------------------------------------------ */

int32_t tc_text_char_width(uint32_t cp, tc_text_mode mode, bool ambiguous_wide);
int32_t tc_text_grapheme_width(const tc_grapheme* g, tc_text_mode mode, bool ambiguous_wide);
int32_t tc_text_string_width(const char* utf8, size_t len, tc_text_mode mode, bool ambiguous_wide);

/* --------------------------------------------------------------------------
 * Measure and layout helpers (docs/05 §7)
 * ------------------------------------------------------------------------ */

tc_text_metrics tc_text_measure(const char* utf8, size_t len,
                                tc_text_mode mode, bool ambiguous_wide);

bool tc_text_truncate_columns(const char* utf8, size_t len, int32_t max_columns,
                              tc_text_mode mode, bool ambiguous_wide,
                              size_t* out_bytes, int32_t* out_columns, bool* truncated);

bool tc_text_wrap_next(const char* utf8, size_t len, int32_t max_columns,
                       tc_text_mode mode, bool ambiguous_wide,
                       size_t* out_break_bytes);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_TC_TEXT_H */
