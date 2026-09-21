#ifndef TERMCORE_CONTROL_TERM_INTERNAL_H
#define TERMCORE_CONTROL_TERM_INTERNAL_H

/* Internals shared by the control layer sources. Never installed. */
#include <stdbool.h>
#include <stdint.h>
#include <termcore/tc_memory.h>
#include <termcore/tc_surface.h>
#include <termcore/tc_term.h>

#include <platform/backend.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime features (docs/02 §4). The enum lives in the public header; the
 * control layer owns both the requested and the applied state, because
 * enter()/leave() must agree on what was actually written. */
/* Same struct as the public tc_term_t; the shorter name keeps the internal
 * sources readable. */
typedef struct tc_term tc_term;

struct tc_input;   /* input layer (docs/04); completed in input/input_internal.h */

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
    tc_cursor_shape cursor_shape;       /* requested shape (docs/02 §7) */
    bool            cursor_shape_applied;  /* a non-default shape was written */

    int32_t cols;
    int32_t rows;
    int32_t cols_px;
    int32_t rows_px;
    bool    size_forced;   /* tc_term_set_size on the null backend */

    /* Input layer state (docs/04): event queue, parser, injection buffers.
     * Allocated in create, freed in destroy, zero allocation at runtime. */
    struct tc_input* input;

    /* render-layer state, owned by term (docs/06 §5). front mirrors the last
     * successfully presented frame; outbuf is the reusable escape-sequence
     * scratch buffer. Both are allocated lazily at the first tc_present. */
    tc_cell* front;
    int32_t  front_cols;
    int32_t  front_rows;
    char*    outbuf;
    size_t   outbuf_len;
    size_t   outbuf_cap;
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

/* --------------------------------------------------------------------------
 * Feature sequences (docs/02 §4, §11).
 *
 * Every sequence is a compile-time constant: the restore path must work from
 * an `atexit` hook and from a crash path, where allocating is not an option.
 * Shared by feature.c (enter/leave) and runtime.c (single-feature toggles).
 * ------------------------------------------------------------------------ */
/* 1049 switches to the alternate buffer and saves the cursor. xterm documents
 * it as "clearing it first", but that clear is not reliable: the scrollback
 * survives it and several emulators keep the previous session's leftovers, so
 * the buffer is erased explicitly once it is active.
 *
 * Nothing is ever erased on the way out: 1049l restores the primary buffer and
 * the saved cursor together, which is what protects the shell prompt below. */
#define TC_SEQ_ALT_SCREEN_ON  "\x1b[?1049h"
#define TC_SEQ_ALT_SCREEN_OFF "\x1b[?1049l"

#define TC_SEQ_CURSOR_HIDE "\x1b[?25l"
#define TC_SEQ_CURSOR_SHOW "\x1b[?25h"
#define TC_SEQ_CURSOR_HOME "\x1b[H"

/* DECSCUSR "restore the terminal default" — the only shape sequence on the
 * restore path (compile-time constant, docs/02 §7, §11). */
#define TC_SEQ_CURSOR_SHAPE_DEFAULT "\x1b[0 q"

#define TC_SEQ_ERASE_SCREEN     "\x1b[2J"   /* ED 2: the visible screen */
#define TC_SEQ_ERASE_SCROLLBACK "\x1b[3J"   /* ED 3: the scrollback as well */
#define TC_SEQ_ALT_SCREEN_CLEAR                                                \
    TC_SEQ_ERASE_SCREEN TC_SEQ_ERASE_SCROLLBACK TC_SEQ_CURSOR_HOME

#define TC_SEQ_MOUSE_CLICK_ON  "\x1b[?1000h\x1b[?1006h\x1b[?1015h"
#define TC_SEQ_MOUSE_DRAG_ON   "\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?1015h"
#define TC_SEQ_MOUSE_MOTION_ON "\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h\x1b[?1015h"
#define TC_SEQ_MOUSE_CLICK_OFF  "\x1b[?1000l\x1b[?1006l\x1b[?1015l"
#define TC_SEQ_MOUSE_DRAG_OFF   "\x1b[?1000l\x1b[?1002l\x1b[?1006l\x1b[?1015l"
#define TC_SEQ_MOUSE_MOTION_OFF "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1015l"

#define TC_SEQ_FOCUS_ON  "\x1b[?1004h"
#define TC_SEQ_FOCUS_OFF "\x1b[?1004l"
#define TC_SEQ_PASTE_ON  "\x1b[?2004h"
#define TC_SEQ_PASTE_OFF "\x1b[?2004l"
#define TC_SEQ_KITTY_ON  "\x1b[>1u"
#define TC_SEQ_KITTY_OFF "\x1b[<u"
#define TC_SEQ_SYNC_ON   "\x1b[?2026h"
#define TC_SEQ_SYNC_OFF  "\x1b[?2026l"

/* The sequence set for a mouse mode, or NULL when the mode is OFF. */
const char* tc_mouse_sequence(tc_mouse_mode mode, bool on);

/* DECSCUSR Ps for a tc_cursor_shape (docs/02 §7): enum index -> Ps.
 * Steady shapes use even Ps (2/4/6), blinking ones odd (1/3/5). */
#define TC_DECSCUSR_PS(shape)                                                  \
    ((const uint8_t[]){ 0, 2, 4, 6, 1, 3, 5 }[(unsigned)(shape)])

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_CONTROL_TERM_INTERNAL_H */
