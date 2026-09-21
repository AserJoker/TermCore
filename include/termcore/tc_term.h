#ifndef TERMCORE_TC_TERM_H
#define TERMCORE_TC_TERM_H

/* Control layer: terminal session lifecycle (docs/02 §2, §3).
 *
 * A session walks CREATED -> ACTIVE -> CREATED:
 *
 *   tc_init_term_options(&opt)   fill defaults, then tweak individual fields
 *   tc_term_create(&opt, &term)  allocate the handle, pick a backend, read the
 *                                size — the terminal itself is NOT touched yet
 *   tc_term_enter(term)          save the original terminal state and apply
 *                                every requested runtime feature
 *   tc_term_leave(term)          undo that in reverse order (idempotent)
 *   tc_term_destroy(term)        leave + release the handle
 *
 * `tc_term_leave` is also reachable from an `atexit` hook, so a session that
 * is never left explicitly still restores the terminal (docs/02 §11).
 *
 * Fields of `tc_term_options` fall into two groups (docs/02 §4): runtime
 * features (applied by `enter`, reversible by `leave`) and init-only
 * attributes (frozen by `create`; changing one means recreating the session).
 */
#include <stdbool.h>
#include <stdint.h>
#include <termcore/tc_export.h>
#include <termcore/tc_memory.h>
#include <termcore/tc_status.h>
#include <termcore/tc_text.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tc_term tc_term_t;

/* Defined by the capability layer (docs/03). The control layer only ever
 * stores the pointer, so it is declared here and completed there. */
typedef struct tc_caps_profile tc_caps_profile;

/* Mouse reporting is four-valued, not on/off (docs/02 §4.1). */
typedef enum tc_mouse_mode {
    TC_MOUSE_OFF    = 0,   /* no reporting */
    TC_MOUSE_CLICK  = 1,   /* 1000: press / release / wheel */
    TC_MOUSE_DRAG   = 2,   /* 1000 + 1002: + drag */
    TC_MOUSE_MOTION = 3    /* 1000 + 1002 + 1003: + any motion */
} tc_mouse_mode;

/* Runtime features (docs/02 §4.1). Each is applied by `enter`, togglable at
 * runtime, and undone by `leave` in reverse order (docs/02 §11). */
typedef enum tc_feature {
    TC_FEATURE_RAW_MODE = 0,
    TC_FEATURE_ALT_SCREEN,
    TC_FEATURE_MOUSE,           /* on = enable mouse reporting; see tc_mouse_mode */
    TC_FEATURE_FOCUS_EVENTS,    /* 1004 */
    TC_FEATURE_BRACKETED_PASTE, /* 2004 */
    TC_FEATURE_KITTY_KEYBOARD,  /* progressive; degrades on probe failure */
    TC_FEATURE_SYNC_UPDATE,     /* 2026 */
    TC_FEATURE_CAPTURE_CTRL_C,  /* CTRL_C arrives as an event, not a signal */
    TC_FEATURE_COUNT
} tc_feature;

/* Cursor shape (docs/02 §7), driven by DECSCUSR. */
typedef enum tc_cursor_shape {
    TC_CURSOR_DEFAULT = 0,   /* terminal default */
    TC_CURSOR_BLOCK,         /* steady block */
    TC_CURSOR_UNDERLINE,     /* steady underline */
    TC_CURSOR_BAR,           /* steady bar */
    TC_CURSOR_BLINK_BLOCK,
    TC_CURSOR_BLINK_UNDERLINE,
    TC_CURSOR_BLINK_BAR
} tc_cursor_shape;

/* What to do when the event queue is full (docs/04 §4). */
typedef enum tc_event_overflow {
    TC_OVERFLOW_DROP_OLDEST = 0,   /* keep the newest events */
    TC_OVERFLOW_DROP_NEWEST = 1    /* keep history, report TC_ERR_OVERFLOW */
} tc_event_overflow;

typedef struct tc_term_options {
    /* —— runtime features: initial values (docs/02 §4) —— */
    bool            raw_mode;             /* default true  */
    bool            alternate_screen;     /* default true  */
    tc_mouse_mode   mouse_mode;           /* default TC_MOUSE_OFF */
    bool            focus_events;         /* default false */
    bool            bracketed_paste;      /* default true  */
    bool            kitty_keyboard;       /* default true (degrades on failure) */
    bool            hide_cursor;          /* default true  */
    bool            capture_ctrl_c;       /* default true: CTRL_C as an event */
    bool            sync_update;          /* default true when supported */

    /* —— init-only attributes: read by create and frozen —— */
    bool                    headless;           /* default false */
    bool                    query_capabilities; /* default true  */
    const tc_caps_profile*  caps_profile;       /* default NULL: skip probing */
    bool                    strict_diff;        /* default false */
    tc_text_mode            text_mode;          /* default TC_TEXT_UNICODE */
    bool                    ambiguous_wide;     /* default false */

    /* —— init-only: time and capacity —— */
    int32_t             capability_timeout_ms;  /* default 200 */
    int32_t             esc_timeout_ms;         /* default 100 */
    int32_t             read_batch_max_bytes;   /* default 4096; 0 = default */
    int32_t             event_queue_capacity;   /* default 256; 0 = default */
    tc_event_overflow   overflow;               /* default DROP_OLDEST */

    /* —— pluggable —— */
    const tc_allocator* allocator;              /* NULL = library default */

    uint32_t reserved[4];
} tc_term_options;

/* Fills *opt with defaults (NULL is a no-op). Must be called before tweaking
 * individual fields; an uninitialized options struct has undefined behaviour.
 * The library deliberately uses no cb_size field (docs/02 §2). */
TC_API void tc_init_term_options(tc_term_options* opt);

/* Creates a session. The terminal is left untouched until `tc_term_enter`.
 *
 *   opt == NULL            all-default options
 *   !headless && !isatty   TC_ERR_NOT_A_TTY
 *   backend failure        TC_ERR_BACKEND
 *   allocation failure     TC_ERR_NOMEM
 *
 * On failure *out is set to NULL. */
TC_API tc_status tc_term_create(const tc_term_options* opt, tc_term_t** out);

/* Saves the original terminal state and applies the requested runtime
 * features. Returns TC_ERR_STATE when the session is already active. */
TC_API tc_status tc_term_enter(tc_term_t* t);

/* Undoes `tc_term_enter` in reverse order; idempotent, so calling it on a
 * session that is not active simply returns TC_OK. */
TC_API tc_status tc_term_leave(tc_term_t* t);

/* Re-enters a session after a suspend/resume cycle (docs/02 §9). The signal
 * layer restores the terminal on SIGTSTP without touching the session state;
 * once the application observes the SIGCONT resume flag it calls this to
 * apply every requested feature again. Safe to call on a session that is not
 * active (it simply enters). */
TC_API tc_status tc_term_reenter(tc_term_t* t);

/* Leaves (if needed) and releases the handle. NULL is a no-op. */
TC_API void tc_term_destroy(tc_term_t* t);

TC_API tc_status tc_term_is_active(const tc_term_t* t, bool* active);

/* --------------------------------------------------------------------------
 * Runtime features (docs/02 §4–§7, §10, §12)
 *
 * In CREATED state these only record the requested state, which `enter`
 * applies in one pass. In ACTIVE state they take effect immediately: the
 * sequence is written and the applied state updated. Setting the current
 * value is a no-op (idempotent, no bytes written). Requested state survives
 * `leave`, so a second `enter` re-applies it (docs/02 §4.2).
 * ------------------------------------------------------------------------ */

/* Toggles one runtime feature. TC_FEATURE_MOUSE=true is "mode != OFF". */
TC_API tc_status tc_term_set_feature(tc_term_t* t, tc_feature f, bool on);

/* `requested` is what the application asked for; `effective` additionally
 * requires capability support (currently equal to `requested` until the
 * capability layer lands, docs/03). Either out pointer may be NULL. */
TC_API tc_status tc_term_get_feature(const tc_term_t* t, tc_feature f,
                                     bool* requested, bool* effective);

/* Mouse mode is four-valued; setting OFF is equivalent to
 * set_feature(MOUSE, false). */
TC_API tc_status tc_term_set_mouse_mode(tc_term_t* t, tc_mouse_mode mode);
TC_API tc_status tc_term_get_mouse_mode(const tc_term_t* t, tc_mouse_mode* mode);

/* Convenience entries sharing the same state as the feature API (docs/02 §5). */
TC_API tc_status tc_term_set_raw(tc_term_t* t, bool on);
TC_API tc_status tc_term_is_raw(const tc_term_t* t, bool* on);

/* docs/02 §6: enter writes 1049h + explicit erase; leave writes only 1049l. */
TC_API tc_status tc_term_enter_alt_screen(tc_term_t* t);
TC_API tc_status tc_term_leave_alt_screen(tc_term_t* t);

/* Cursor visibility and shape (docs/02 §7); position is 0-based cell coords
 * and goes through the backend. */
TC_API tc_status tc_term_set_cursor_visible(tc_term_t* t, bool visible);
TC_API tc_status tc_term_set_cursor_shape(tc_term_t* t, tc_cursor_shape shape);
TC_API tc_status tc_term_set_cursor_pos(tc_term_t* t, int32_t x, int32_t y);

/* docs/02 §10: OSC 0 title; unsupported backends report TC_ERR_UNSUPPORTED. */
TC_API tc_status tc_term_set_title(tc_term_t* t, const char* utf8);

/* Text mode is a term-level default; a surface created afterwards reads the
 * current value (docs/02 §12). */
TC_API tc_status tc_term_set_text_mode(tc_term_t* t, tc_text_mode mode);
TC_API tc_status tc_term_get_text_mode(const tc_term_t* t, tc_text_mode* mode);
TC_API tc_status tc_term_set_ambiguous_wide(tc_term_t* t, bool wide);

/* Columns and rows of the terminal. The size is refreshed from the backend on
 * every call (docs/02 §8). */
TC_API tc_status tc_term_get_size(const tc_term_t* t, int32_t* cols, int32_t* rows);

/* Pixel size; 0 when the backend cannot report it (docs/08 §2.5). */
TC_API tc_status tc_term_get_size_px(const tc_term_t* t, int32_t* w, int32_t* h);

/* Overrides the reported size. Only meaningful for the null (headless)
 * backend, where it is the way to test large and small screens (docs/02 §3). */
TC_API tc_status tc_term_set_size(tc_term_t* t, int32_t cols, int32_t rows);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_TC_TERM_H */
