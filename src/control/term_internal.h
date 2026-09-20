#ifndef TERMCORE_CONTROL_TERM_INTERNAL_H
#define TERMCORE_CONTROL_TERM_INTERNAL_H

/* Internals shared by the control layer sources. Never installed. */
#include <stdbool.h>
#include <stdint.h>
#include <termcore/tc_memory.h>
#include <termcore/tc_term.h>

#include <platform/backend.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime features (docs/02 §4). The public setters land with the feature
 * API; until then the control layer owns both the requested and the applied
 * state, because enter()/leave() must agree on what was actually written. */
/* Same struct as the public tc_term_t; the shorter name keeps the internal
 * sources readable. */
typedef struct tc_term tc_term;

typedef enum tc_feature {
    TC_FEATURE_RAW_MODE = 0,
    TC_FEATURE_ALT_SCREEN,
    TC_FEATURE_MOUSE,
    TC_FEATURE_FOCUS_EVENTS,
    TC_FEATURE_BRACKETED_PASTE,
    TC_FEATURE_KITTY_KEYBOARD,
    TC_FEATURE_SYNC_UPDATE,
    TC_FEATURE_CAPTURE_CTRL_C,
    TC_FEATURE_COUNT
} tc_feature;

typedef enum tc_term_state {
    TC_TERM_STATE_CREATED = 0,
    TC_TERM_STATE_ACTIVE,
    TC_TERM_STATE_DESTROYED
} tc_term_state;

struct tc_term {
    const tc_allocator* alloc;
    tc_backend*         backend;
    tc_term_t*          next_live;   /* intrusive list behind the atexit hook */

    tc_term_state   state;
    tc_backend_kind backend_kind;

    /* init-only attributes (docs/02 §4) */
    bool                headless;
    bool                query_capabilities;
    bool                strict_diff;
    tc_text_mode        text_mode;
    bool                ambiguous_wide;
    int32_t             capability_timeout_ms;
    int32_t             esc_timeout_ms;
    int32_t             read_batch_max_bytes;
    int32_t             event_queue_capacity;
    tc_event_overflow   overflow;

    /* runtime features: requested vs. actually applied */
    bool          requested[TC_FEATURE_COUNT];
    bool          applied[TC_FEATURE_COUNT];
    tc_mouse_mode mouse_mode;
    tc_mouse_mode applied_mouse_mode;
    bool          hide_cursor;
    bool          cursor_hidden;

    int32_t cols;
    int32_t rows;
    int32_t cols_px;
    int32_t rows_px;
    bool    size_forced;   /* tc_term_set_size on the null backend */
};

/* control/feature.c — apply on enter, undo in reverse on leave. */
tc_status tc_term_apply_features(tc_term* t);
tc_status tc_term_restore_features(tc_term* t);

/* Async-signal-safe restore: writes the closing sequences and restores raw
 * mode, but never mutates state / applied[] (docs/02 §11). The signal module
 * walks every ACTIVE live term with it. */
tc_status tc_term_restore_signal(tc_term* t);

/* Read-only walk of the live-term registry (signal module). */
const tc_term* tc_term_live_first(void);
const tc_term* tc_term_live_next(const tc_term* t);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_CONTROL_TERM_INTERNAL_H */
