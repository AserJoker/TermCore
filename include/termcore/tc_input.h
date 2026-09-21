#ifndef TERMCORE_TC_INPUT_H
#define TERMCORE_TC_INPUT_H

/* Input layer: byte stream -> structured events (docs/04).
 *
 * The library owns no event loop and never sleeps or busy-waits: events are
 * pulled by the application through the three pull primitives, and the greedy
 * read (docs/04 §4.4) plus the ESC deadline (docs/04 §11) disambiguate a lone
 * ESC from a sequence prefix without blocking.
 *
 *   tc_wait_event  OS-level wait (or immediate), then fill
 *   tc_peek_event  look at the head of the queue without consuming
 *   tc_get_event   consume one event
 *   tc_term_flush_pending  settle a pending lone ESC / partial sequence now
 *
 * Events are POD with a fixed layout; `paste.text` is a borrowed pointer that
 * is only valid until the next event is taken. All events carry a monotonic
 * `seq` for tests and dedup.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termcore/tc_export.h>
#include <termcore/tc_status.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tc_term tc_term_t;

/* --------------------------------------------------------------------------
 * Key codes and modifiers (docs/04 §3)
 * ------------------------------------------------------------------------ */

typedef enum tc_key {
    TC_KEY_NONE = 0,
    /* editing */
    TC_KEY_ENTER, TC_KEY_TAB, TC_KEY_BACKSPACE, TC_KEY_ESCAPE, TC_KEY_DELETE,
    TC_KEY_INSERT, TC_KEY_HOME, TC_KEY_END, TC_KEY_PAGE_UP, TC_KEY_PAGE_DOWN,
    /* arrows */
    TC_KEY_UP, TC_KEY_DOWN, TC_KEY_LEFT, TC_KEY_RIGHT,
    /* function keys */
    TC_KEY_F1, TC_KEY_F2, TC_KEY_F3, TC_KEY_F4, TC_KEY_F5, TC_KEY_F6,
    TC_KEY_F7, TC_KEY_F8, TC_KEY_F9, TC_KEY_F10, TC_KEY_F11, TC_KEY_F12,
    /* keypad (when distinguishable) */
    TC_KEY_KP_0, TC_KEY_KP_1, TC_KEY_KP_2, TC_KEY_KP_3, TC_KEY_KP_4,
    TC_KEY_KP_5, TC_KEY_KP_6, TC_KEY_KP_7, TC_KEY_KP_8, TC_KEY_KP_9,
    TC_KEY_KP_ENTER,
    /* the modifiers themselves */
    TC_KEY_SHIFT, TC_KEY_CTRL, TC_KEY_ALT, TC_KEY_SUPER,
    TC_KEY_COUNT
} tc_key;

typedef enum tc_mod {
    TC_MOD_NONE  = 0,
    TC_MOD_SHIFT = 1u << 0,
    TC_MOD_ALT   = 1u << 1,   /* Meta / Option */
    TC_MOD_CTRL  = 1u << 2,
    TC_MOD_SUPER = 1u << 3,   /* Win / Cmd */
    TC_MOD_HYPER = 1u << 4,
    TC_MOD_META  = 1u << 5,
    TC_MOD_CAPS  = 1u << 6,
    TC_MOD_NUM   = 1u << 7
} tc_mod;

/* --------------------------------------------------------------------------
 * Mouse (docs/04 §9)
 * ------------------------------------------------------------------------ */

typedef enum tc_mouse_button {
    TC_MOUSE_NONE   = 0,
    TC_MOUSE_LEFT   = 1,
    TC_MOUSE_MIDDLE = 2,
    TC_MOUSE_RIGHT  = 3,
    TC_MOUSE_WHEEL  = 4,
    TC_MOUSE_X1     = 5,
    TC_MOUSE_X2     = 6
} tc_mouse_button;

typedef enum tc_mouse_action {
    TC_MOUSE_ACT_DOWN = 1,
    TC_MOUSE_ACT_UP,
    TC_MOUSE_ACT_DRAG,
    TC_MOUSE_ACT_MOVE,
    TC_MOUSE_ACT_WHEEL_UP,
    TC_MOUSE_ACT_WHEEL_DOWN
} tc_mouse_action;

/* --------------------------------------------------------------------------
 * Event model (docs/04 §2)
 * ------------------------------------------------------------------------ */

typedef enum tc_event_kind {
    TC_EV_KEY    = 1,
    TC_EV_MOUSE  = 2,
    TC_EV_RESIZE = 3,
    TC_EV_FOCUS  = 4,
    TC_EV_PASTE  = 5,
    TC_EV_QUIT   = 6    /* optional: SIGTERM / CTRL_BREAK exit requests */
} tc_event_kind;

/* Reasons for a TC_EV_QUIT (docs/04 §2). */
enum {
    TC_QUIT_SIGTERM     = 1,
    TC_QUIT_SIGHUP      = 2,
    TC_QUIT_CTRL_BREAK  = 3
};

typedef struct tc_event {
    tc_event_kind kind;
    uint32_t      seq;          /* monotonic per-session sequence number */
    union {
        struct {
            uint32_t codepoint;     /* printable character; 0 when none */
            tc_key   key;           /* function key; TC_KEY_NONE when none */
            uint16_t mods;          /* TC_MOD_* bitwise-or */
            bool     is_release;    /* key release (Kitty only) */
        } key;

        struct {
            int32_t          x, y;        /* 0-based cell coordinates */
            tc_mouse_button  button;
            tc_mouse_action  action;
            uint16_t         mods;
        } mouse;

        struct {
            int32_t cols, rows;
            int32_t pixel_w, pixel_h;     /* 0 when unavailable */
        } resize;

        struct {
            bool focused;
        } focus;

        struct {
            const char* text;             /* borrowed; valid until next take */
            size_t      len;
        } paste;

        struct {
            int32_t reason;               /* TC_QUIT_* */
        } quit;
    } u;
    uint32_t reserved[2];
} tc_event;

/* --------------------------------------------------------------------------
 * Pull primitives (docs/04 §4, §16)
 * ------------------------------------------------------------------------ */

typedef struct tc_input_source_vtable tc_input_source_vtable;

/* Blocks at most `timeout_ms` waiting for input to arrive, then fills and
 * reports whether an event is ready. timeout_ms=0 is non-blocking, <0 waits
 * indefinitely. Returns TC_OK even on timeout (ready=false); TC_ERR_STATE
 * when the session is not active. The effective wait is capped by the ESC
 * deadline so a pending lone ESC is settled in time (docs/04 §11). */
TC_API tc_status tc_wait_event(tc_term_t* t, int32_t timeout_ms, bool* ready);

/* Fills `out` with the head of the queue without consuming it. */
TC_API tc_status tc_peek_event(tc_term_t* t, tc_event* out, bool* has);

/* Consumes one event. */
TC_API tc_status tc_get_event(tc_term_t* t, tc_event* out, bool* has);

/* Settles any pending residual (a lone ESC -> TC_KEY_ESCAPE, a partial
 * sequence -> discarded) right now, without waiting (docs/04 §11). */
TC_API tc_status tc_term_flush_pending(tc_term_t* t, bool* has);

/* --------------------------------------------------------------------------
 * Injection and input-source takeover (docs/04 §5)
 * ------------------------------------------------------------------------ */

/* Feeds bytes through the real parser (test value: identical path to a real
 * terminal). May be split arbitrarily. */
TC_API tc_status tc_term_inject_input(tc_term_t* t, const void* bytes, size_t len);

/* Queues a ready-made event, bypassing the parser. */
TC_API tc_status tc_term_inject_event(tc_term_t* t, const tc_event* ev);

/* A custom input source takes over reading from the default terminal source.
 * Pass vt=NULL to restore the default. The previous source's dispose() is
 * called once. */
TC_API tc_status tc_term_set_input_source(tc_term_t* t,
                                          const tc_input_source_vtable* vt,
                                          void* ctx);

typedef struct tc_input_source_vtable {
    /* OS-level wait; TC_OK + ready=false on timeout; must not busy-wait. */
    tc_status (*wait_ready)(void* ctx, int32_t timeout_ms, bool* ready);
    /* Non-blocking read; nread=0 ends the greedy loop; may split freely. */
    tc_status (*read)(void* ctx, void* buf, size_t cap, size_t* nread);
    /* Called once when the source is replaced or the session is destroyed. */
    void      (*dispose)(void* ctx);
} tc_input_source_vtable;

/* --------------------------------------------------------------------------
 * Diagnostics (docs/04 §16)
 * ------------------------------------------------------------------------ */

/* Stable key name, e.g. "UP"; returns TC_ERR_INVALID_ARG for out-of-range. */
TC_API tc_status tc_key_name(tc_key k, const char** out);

/* Composes modifier names, e.g. "CTRL+ALT"; empty string for none. */
TC_API tc_status tc_mods_name(uint16_t mods, char* buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_TC_INPUT_H */
