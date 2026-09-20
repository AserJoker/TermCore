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
