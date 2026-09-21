#ifndef TERMCORE_TC_CAPS_H
#define TERMCORE_TC_CAPS_H

/* Capability layer (docs/03): answers "what does the current terminal
 * support" and drives the render / input degradation.
 *
 * Detection priority (docs/03 §2):
 *   1. user-confirmed conclusions (probe / cached profile) and explicit
 *      overrides (API / TERMCORE_* env vars)      — highest
 *   2. environment inference (TERM / COLORTERM / NO_COLOR / CI / TERM_PROGRAM)
 *   3. DA query (when query_capabilities and a TTY)
 *   4. terminfo / termcap lookup (by TERM)
 *   5. built-in TERM database (hard-coded common terminals)
 *   6. conservative default (lowest capability set)
 *
 * The first conclusive step wins; no step may block startup (the DA query is
 * time-boxed). When in doubt the library under-estimates: low support only
 * affects looks, over-estimation produces garbage.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termcore/tc_export.h>
#include <termcore/tc_input.h>   /* tc_event (probe feed) */
#include <termcore/tc_status.h>
#include <termcore/tc_surface.h> /* tc_rect */
#include <termcore/tc_term.h>    /* tc_mouse_mode (tc_caps.mouse) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tc_term tc_term_t;
typedef struct tc_surface tc_surface_t;

/* --------------------------------------------------------------------------
 * Capability bitset (docs/03 §1)
 * ------------------------------------------------------------------------ */

typedef enum tc_cap {
    /* color */
    TC_CAP_COLOR_16       = 1u << 0,
    TC_CAP_COLOR_256      = 1u << 1,
    TC_CAP_TRUECOLOR      = 1u << 2,

    /* screen and cursor */
    TC_CAP_ALT_SCREEN     = 1u << 3,
    TC_CAP_CURSOR_SHAPE   = 1u << 4,
    TC_CAP_CURSOR_HIDE    = 1u << 5,
    TC_CAP_TITLE          = 1u << 6,
    TC_CAP_SYNC_UPDATE    = 1u << 7,   /* mode 2026 */

    /* styles */
    TC_CAP_STYLE_BOLD     = 1u << 8,
    TC_CAP_STYLE_DIM      = 1u << 9,
    TC_CAP_STYLE_ITALIC   = 1u << 10,
    TC_CAP_STYLE_UNDERLINE = 1u << 11,
    TC_CAP_STYLE_BLINK    = 1u << 12,
    TC_CAP_STYLE_REVERSE  = 1u << 13,
    TC_CAP_STYLE_STRIKE   = 1u << 14,
    TC_CAP_STYLE_UNDERCURL = 1u << 15,

    /* input */
    TC_CAP_MOUSE_X10      = 1u << 16,  /* 1000 */
    TC_CAP_MOUSE_DRAG     = 1u << 17,  /* 1002 */
    TC_CAP_MOUSE_MOVE     = 1u << 18,  /* 1003 */
    TC_CAP_MOUSE_SGR      = 1u << 19,  /* 1006 */
    TC_CAP_FOCUS_EVENTS   = 1u << 20,  /* 1004 */
    TC_CAP_BRACKETED_PASTE = 1u << 21, /* 2004 */
    TC_CAP_KITTY_KEYBOARD = 1u << 22,  /* progressive keyboard protocol */
    TC_CAP_MODIFY_OTHER   = 1u << 23,  /* modifyOtherKeys (xterm) */

    /* text */
    TC_CAP_UNICODE        = 1u << 24,  /* wide chars render correctly */
    TC_CAP_PIXEL_SIZE     = 1u << 25,  /* pixel size is queryable */

    TC_CAP_NONE           = 0
} tc_cap;

typedef enum tc_color_level {
    TC_COLOR_LEVEL_MONO = 0,   /* no color: all colors ignored */
    TC_COLOR_LEVEL_16,         /* ANSI 16 (incl. bright) */
    TC_COLOR_LEVEL_256,        /* xterm-256color indexed */
    TC_COLOR_LEVEL_TRUECOLOR   /* 24-bit truecolor */
} tc_color_level;

typedef struct tc_caps {
    uint64_t       flags;       /* tc_cap bitwise-or */
    tc_color_level color;       /* docs/03 §3 */
    int32_t        max_colors;  /* 16 / 256 / 0 (truecolor: unlimited) */
    tc_mouse_mode  mouse;       /* best available mouse mode (docs/02) */
    uint32_t       reserved[2];
} tc_caps;

/* Query (docs/03 §1). */
TC_API tc_status tc_term_get_caps(const tc_term_t* t, tc_caps* out);
TC_API bool      tc_caps_has(const tc_caps* c, tc_cap flag);
TC_API tc_status tc_term_get_color_level(const tc_term_t* t, tc_color_level* out);

/* --------------------------------------------------------------------------
 * Overrides and environment inference (docs/03 §4, §9)
 * ------------------------------------------------------------------------ */

/* Overrides the capability conclusion; NULL clears the override. After an
 * override every layer (render degradation, input protocol gating, and the
 * `effective` of the runtime toggles, docs/02 §4) works off the overridden
 * value. */
TC_API tc_status tc_term_set_caps_override(tc_term_t* t, const tc_caps* caps);

/* Derives capabilities from environment variables only (TERM / COLORTERM /
 * NO_COLOR / TERMCORE_* / TERM_PROGRAM), for tests and forced setups. */
TC_API tc_status tc_caps_from_env(tc_caps* out);

/* Color degradation, pure functions (docs/03 §3). */
TC_API tc_status tc_caps_downgrade_rgb(const tc_caps* c, uint8_t r, uint8_t g, uint8_t b,
                                       tc_color* out);
TC_API tc_status tc_caps_downgrade_indexed(const tc_caps* c, uint8_t idx256,
                                           tc_color* out);

/* --------------------------------------------------------------------------
 * Profile (docs/03 §10): per-bit provenance, confirmation and persistence.
 * ------------------------------------------------------------------------ */

typedef enum tc_caps_origin {
    TC_CAPS_ORIGIN_DEFAULT = 0,  /* conservative default */
    TC_CAPS_ORIGIN_BUILTIN,      /* built-in TERM database */
    TC_CAPS_ORIGIN_TERMINFO,     /* terminfo / termcap */
    TC_CAPS_ORIGIN_INFER,        /* environment inference */
    TC_CAPS_ORIGIN_QUERY,        /* DA query */
    TC_CAPS_ORIGIN_OVERRIDE,     /* explicit override / env force */
    TC_CAPS_ORIGIN_PROBE         /* user-confirmed probe conclusion */
} tc_caps_origin;

typedef struct tc_caps_entry {
    uint64_t       bit;
    bool           supported;
    tc_caps_origin origin;
    bool           user_confirmed;   /* confirmed through human interaction */
    int64_t        decided_at_ms;
    uint32_t       reserved[2];
} tc_caps_entry;

/* Environment fingerprint: "is a saved conclusion still valid?" */
typedef struct tc_caps_fingerprint {
    char     term[64];          /* TERM */
    char     term_program[64];  /* TERM_PROGRAM / terminal name */
    char     colorterm[32];     /* COLORTERM */
    int32_t  da_primary_id;     /* DA primary response */
    int32_t  da_version;        /* secondary DA version, if any */
    uint32_t lib_abi_version;
    uint64_t profile_version;   /* profile format version */
    int64_t  saved_at_ms;
    uint32_t reserved[2];
} tc_caps_fingerprint;

typedef struct tc_caps_profile {
    uint64_t       flags;        /* conclusion bitset (same as tc_caps.flags) */
    tc_caps        caps;         /* derived fields: color / max_colors / mouse */
    tc_caps_entry* entries;      /* per-bit detail, library-allocated */
    size_t         entry_count;
    tc_caps_fingerprint fp;
    uint64_t       reserved[2];
} tc_caps_profile;

/* Get / write (docs/03 §10.2). The profile is a snapshot: `entries` is
 * library-allocated and released by tc_caps_profile_dispose. */
TC_API tc_status tc_term_get_caps_profile(const tc_term_t* t, tc_caps_profile* out);
TC_API void      tc_caps_profile_dispose(tc_caps_profile* p);
TC_API tc_status tc_caps_get_entry(const tc_caps_profile* p, uint64_t bit,
                                   const tc_caps_entry** e);
TC_API tc_status tc_term_get_caps_fingerprint(const tc_term_t* t,
                                              tc_caps_fingerprint* out);

/* Incremental bit change / full replace. set_caps_bits only touches the given
 * bits; apply_caps_profile replaces everything. Writes carry an origin;
 * PROBE and OVERRIDE outrank inference and query results (docs/03 §2). */
TC_API tc_status tc_term_set_caps_bits(tc_term_t* t, uint64_t clear, uint64_t set,
                                       tc_caps_origin origin);
TC_API tc_status tc_term_apply_caps_profile(tc_term_t* t, const tc_caps_profile* p);

/* Serialization; the library never does file I/O (docs/03 §10.3). Pass
 * cap=0 / NULL to query the required size into *need. */
TC_API tc_status tc_caps_profile_serialize(const tc_caps_profile* p,
                                           void* buf, size_t cap, size_t* need);
TC_API tc_status tc_caps_profile_deserialize(const void* buf, size_t len,
                                             tc_caps_profile* out);
TC_API tc_status tc_caps_profile_to_text(const tc_caps_profile* p,
                                         char* buf, size_t cap, size_t* need);
TC_API tc_status tc_caps_profile_from_text(const char* text, size_t len,
                                           tc_caps_profile* out);
/* Fingerprint comparison: a mismatch is not an error, it only means the
 * environment may have changed (docs/03 §10.3). */
TC_API tc_status tc_caps_profile_match(const tc_caps_profile* p,
                                       const tc_caps_fingerprint* now, bool* ok);

/* --------------------------------------------------------------------------
 * Interactive probe (docs/03 §11)
 * ------------------------------------------------------------------------ */

typedef enum tc_caps_probe_kind {
    TC_PROBE_AUTO = 0,   /* the library judges: query or fed events suffice */
    TC_PROBE_VISUAL,     /* render a sample, user picks one of several answers */
    TC_PROBE_CONFIRM     /* render a sample, user answers yes / no */
} tc_caps_probe_kind;

typedef struct tc_caps_probe_item {
    uint64_t           bit;           /* related tc_cap bits (may be several) */
    const char*        id;            /* stable id: "color.level", "mouse.sgr" … */
    const char*        title;         /* default title (English) */
    const char*        question;      /* default question */
    tc_caps_probe_kind kind;
    int32_t            choice_count;  /* VISUAL items: number of answers */
    const char* const* choices;       /* answer labels */
    uint32_t           reserved[2];
} tc_caps_probe_item;

/* Static catalog, no free needed (docs/03 §11.1). */
TC_API const tc_caps_probe_item* tc_caps_probe_catalog(size_t* count);

/* Draws the sample for one capability into `area` of the surface. Pure
 * drawing: no capability change, no terminal writes, no events. With
 * TC_CAPS_SAMPLE_RAW the sample bypasses color degradation so unsupported
 * styles can still be probed (docs/03 §11.2). */
#define TC_CAPS_SAMPLE_RAW 1u
TC_API tc_status tc_caps_render_sample(tc_surface_t* s, tc_rect area,
                                       uint64_t bit, uint32_t flags);

typedef struct tc_caps_probe tc_caps_probe;

TC_API tc_status tc_caps_probe_begin(tc_term_t* t, uint64_t bits, uint32_t flags,
                                     tc_caps_probe** out);
TC_API tc_status tc_caps_probe_current(const tc_caps_probe* p, size_t* index,
                                       size_t* total, const tc_caps_probe_item** item);
TC_API tc_status tc_caps_probe_render(tc_caps_probe* p, tc_surface_t* s, tc_rect area);
TC_API tc_status tc_caps_probe_feed(tc_caps_probe* p, const tc_event* ev);
TC_API tc_status tc_caps_probe_answer(tc_caps_probe* p, int32_t choice);
TC_API tc_status tc_caps_probe_skip(tc_caps_probe* p);
TC_API tc_status tc_caps_probe_back(tc_caps_probe* p);
TC_API tc_status tc_caps_probe_finish(tc_caps_probe* p, tc_caps* out);
TC_API void      tc_caps_probe_abort(tc_caps_probe* p);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_TC_CAPS_H */
