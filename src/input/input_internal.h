#ifndef TERMCORE_INPUT_INPUT_INTERNAL_H
#define TERMCORE_INPUT_INPUT_INTERNAL_H

/* Internals of the input layer (docs/04). Never installed. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termcore/tc_input.h>

#include <control/term_internal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parser states (docs/04 §6). PASTE is the bracketed-paste payload state,
 * where every byte is accumulated verbatim instead of being parsed. */
typedef enum tc_in_state {
    TC_IN_GROUND = 0,
    TC_IN_UTF8,      /* collecting a multi-byte UTF-8 sequence */
    TC_IN_ESC,
    TC_IN_CSI,
    TC_IN_SS3,
    TC_IN_OSC,
    TC_IN_DCS,
    TC_IN_X10,       /* X10 mouse: Cb Cx Cy payload bytes after CSI M */
    TC_IN_PASTE,
    TC_IN_STATE_COUNT
} tc_in_state;

/* Fixed parser limits (docs/04 §6 robustness rules). */
#define TC_IN_PARAMS_MAX    16
#define TC_IN_SEQ_MAX       256
#define TC_IN_UTF8_MAX      4
#define TC_IN_INJECT_CAP    16384u      /* injected-byte queue cap */
#define TC_IN_INJECT_EVENTS 64          /* injected-event queue cap */
#define TC_IN_PASTE_CAP     65536u      /* bracketed-paste accumulation cap */

typedef struct tc_input tc_input;

struct tc_input {
    /* Event queue: fixed-capacity ring, the single exit for every event
     * (docs/04 §13). */
    tc_event* events;
    int32_t   cap;
    int32_t   head;
    int32_t   count;
    uint32_t  seq;              /* next sequence number for library-made events */

    /* Injection (docs/04 §5). Bytes go through the real parser, events bypass
     * it; both are drained into the event queue during fill, bytes first. */
    uint8_t*  inject_bytes;
    size_t    inject_len;
    tc_event* inject_events;
    int32_t   inject_e_head;
    int32_t   inject_e_count;

    /* Parser state machine (docs/04 §6). */
    uint8_t   state;            /* tc_in_state */
    uint8_t   seq_buf[TC_IN_SEQ_MAX];
    size_t    seq_len;
    int32_t   params[TC_IN_PARAMS_MAX];
    int32_t   nparams;
    uint8_t   sep[TC_IN_PARAMS_MAX];  /* ';' or ':' before each param (Kitty) */
    bool      have_prefix;      /* CSI '<' '=' '>' '?' prefix byte */
    uint8_t   prefix;
    bool      csi_drop;         /* oversized/malformed CSI: swallow to final byte */
    uint8_t   utf8[TC_IN_UTF8_MAX];   /* partial UTF-8 sequence */
    size_t    utf8_len;
    uint8_t   utf8_expected;          /* expected total length of the sequence */
    bool      alt_pending;      /* ESC+char: UTF-8 completes an ALT sequence */
    bool      st_esc;           /* OSC/DCS: a 0x1B seen, maybe start of ST */
    uint8_t   x10_buf[3];       /* X10 mouse: Cb Cx Cy payload bytes */
    size_t    x10_len;
    uint8_t   paste_tail[6];    /* last 6 bytes, end-sequence match window */

    /* Bracketed paste accumulation (docs/04 §10). */
    char*     paste_buf;
    size_t    paste_len;   /* bytes stored (capped at TC_IN_PASTE_CAP) */
    size_t    paste_total; /* bytes seen in this paste, incl. dropped */

    /* Lone-ESC / partial-sequence deadline (docs/04 §11): the state the
     * parser stopped in at the end of a batch, and when it got there. */
    uint8_t   residual_state;
    uint64_t  residual_arrive_ms;

    /* Mouse drag tracking (docs/04 §9): the currently held button. */
    tc_mouse_button pressed;
    bool      pressed_valid;

    /* Resize coalescing (docs/04 §12): last size an event was emitted for. */
    int32_t   last_resize_cols;
    int32_t   last_resize_rows;

    /* External input source (docs/04 §5), replaces the default terminal
     * source while set. */
    const tc_input_source_vtable* src_vt;
    void*     src_ctx;
};

/* input.c — lifecycle, queue, pull primitives. */
tc_status tc_input_create(tc_term* t, const tc_allocator* alloc);
void      tc_input_destroy(tc_term* t);
void      tc_input_reset(tc_term* t);   /* enter(): drop residuals, clear drag */
bool      tc_input_queue_empty(const tc_input* in);
tc_status tc_input_enqueue(tc_term* t, const tc_event* ev);
uint64_t  tc_input_now_ms(void);        /* monotonic clock */
/* inject + greedy read + resize + settle; TC_ERR_OVERFLOW when the DROP_NEWEST
 * policy discarded an event during this fill. */
tc_status tc_input_fill(tc_term* t);
tc_status tc_input_settle(tc_term* t, bool forced, bool* produced);

/* parser.c — byte stream -> events. */
tc_status tc_parser_feed(tc_term* t, const uint8_t* bytes, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_INPUT_INPUT_INTERNAL_H */
