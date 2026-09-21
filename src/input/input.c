/* Input layer: pull primitives, event queue and injection (docs/04 §4, §5,
 * §11, §13).
 *
 * The library never sleeps, never busy-waits and owns no background thread:
 * every fill happens on the pull path. A pull is two phases (docs/04 §4.5):
 * fill (drain injected bytes -> injected events -> greedy read the active
 * source -> resize check -> settle the ESC deadline) then dequeue.
 *
 * All buffers are allocated in tc_input_create and never touched on the hot
 * path, so a pull allocates nothing (docs/04 §4.2).
 */
#if !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* clock_gettime on POSIX */
#endif

#include <input/input_internal.h>

#include <platform/backend.h>
#include <control/signal_internal.h>

#include <string.h>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

tc_status tc_input_create(tc_term* t, const tc_allocator* alloc) {
    tc_input* in;

    if (!t || !alloc) return TC_ERR_INVALID_ARG;
    t->input = NULL;

    in = (tc_input*)alloc->alloc(alloc->ctx, sizeof(*in), sizeof(void*));
    if (!in) return TC_ERR_NOMEM;
    memset(in, 0, sizeof(*in));

    in->state          = TC_IN_GROUND;
    in->residual_state = TC_IN_GROUND;

    in->cap = t->event_queue_capacity;
    in->events = (tc_event*)alloc->alloc(alloc->ctx,
                                         (size_t)in->cap * sizeof(tc_event),
                                         sizeof(void*));
    if (!in->events) goto fail_events;

    in->inject_bytes = (uint8_t*)alloc->alloc(alloc->ctx, TC_IN_INJECT_CAP, 1);
    if (!in->inject_bytes) goto fail_inject;

    in->inject_events = (tc_event*)alloc->alloc(alloc->ctx,
                                                TC_IN_INJECT_EVENTS * sizeof(tc_event),
                                                sizeof(void*));
    if (!in->inject_events) goto fail_inject_events;

    in->paste_buf = (char*)alloc->alloc(alloc->ctx, TC_IN_PASTE_CAP, 1);
    if (!in->paste_buf) goto fail_paste;

    /* Seed the resize coalescer with the size create already read, so the
     * first set_size/notify produces an event only when it actually changes. */
    in->last_resize_cols = t->cols;
    in->last_resize_rows = t->rows;

    t->input = in;
    return TC_OK;

fail_paste:
    alloc->free(alloc->ctx, in->inject_events,
                TC_IN_INJECT_EVENTS * sizeof(tc_event));
fail_inject_events:
    alloc->free(alloc->ctx, in->inject_bytes, TC_IN_INJECT_CAP);
fail_inject:
    alloc->free(alloc->ctx, in->events, (size_t)in->cap * sizeof(tc_event));
fail_events:
    alloc->free(alloc->ctx, in, sizeof(*in));
    return TC_ERR_NOMEM;
}

void tc_input_destroy(tc_term* t) {
    tc_input*          in;
    const tc_allocator* alloc;

    if (!t || !t->input) return;
    in    = t->input;
    alloc = t->alloc ? t->alloc : tc_allocator_default();

    if (in->src_vt && in->src_vt->dispose) in->src_vt->dispose(in->src_ctx);

    if (in->paste_buf) alloc->free(alloc->ctx, in->paste_buf, TC_IN_PASTE_CAP);
    if (in->inject_events)
        alloc->free(alloc->ctx, in->inject_events,
                    TC_IN_INJECT_EVENTS * sizeof(tc_event));
    if (in->inject_bytes) alloc->free(alloc->ctx, in->inject_bytes, TC_IN_INJECT_CAP);
    if (in->events) alloc->free(alloc->ctx, in->events,
                                (size_t)in->cap * sizeof(tc_event));
    alloc->free(alloc->ctx, in, sizeof(*in));
    t->input = NULL;
}

/* enter(): the terminal state is (re)established, so parser residuals and the
 * mouse drag state are stale (docs/04 §9). The event queue is kept — queued
 * events belong to the application, not to the session transition. */
void tc_input_reset(tc_term* t) {
    tc_input* in = t->input;

    if (!in) return;

    in->state           = TC_IN_GROUND;
    in->seq_len         = 0;
    in->nparams         = 0;
    in->have_prefix     = false;
    in->prefix          = 0;
    in->csi_drop        = false;
    in->utf8_len        = 0;
    in->utf8_expected   = 0;
    in->alt_pending     = false;
    in->paste_len       = 0;
    in->residual_state  = TC_IN_GROUND;
    in->pressed         = TC_MOUSE_NONE;
    in->pressed_valid   = false;
}

/* --------------------------------------------------------------------------
 * Event queue (docs/04 §13)
 * ------------------------------------------------------------------------ */

bool tc_input_queue_empty(const tc_input* in) {
    return in->count == 0;
}

tc_status tc_input_enqueue(tc_term* t, const tc_event* ev) {
    tc_input* in = t->input;
    int32_t   slot;

    if (!ev) return TC_ERR_INVALID_ARG;

    if (in->count < in->cap) {
        slot = (in->head + in->count) % in->cap;
        in->events[slot] = *ev;
        in->count++;
        return TC_OK;
    }

    if (t->overflow == TC_OVERFLOW_DROP_OLDEST) {
        /* Overwrite the oldest: new events always win (default policy). */
        in->events[in->head] = *ev;
        in->head = (in->head + 1) % in->cap;
        return TC_OK;
    }

    return TC_ERR_OVERFLOW;   /* DROP_NEWEST: keep history, report overflow */
}

/* --------------------------------------------------------------------------
 * Monotonic clock (docs/04 §11 deadline bookkeeping)
 * ------------------------------------------------------------------------ */

uint64_t tc_input_now_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* --------------------------------------------------------------------------
 * Fill: injection -> greedy read -> resize -> settle (docs/04 §4.5)
 * ------------------------------------------------------------------------ */

/* Greedy read: loop the active source until it reports "nothing more" or the
 * per-batch cap is reached (docs/04 §4.4). */
static void input_greedy_read(tc_term* t) {
    tc_input* in = t->input;
    uint8_t   buf[4096];
    size_t    total = 0;

    for (;;) {
        size_t     n = 0;
        tc_status  st;

        if (in->src_vt) {
            if (!in->src_vt->read) break;
            st = in->src_vt->read(in->src_ctx, buf, sizeof(buf), &n);
        } else {
            st = tc_backend_read(t->backend, buf, sizeof(buf), &n);
        }

        if (st != TC_OK || n == 0) break;
        (void)tc_parser_feed(t, buf, n);

        total += n;
        if (total >= (size_t)t->read_batch_max_bytes) break;
    }
}

/* A resize notification (SIGWINCH / tc_term_set_size) is a flag only; the
 * event is generated here, on the pull path, coalesced to size changes
 * (docs/04 §12). */
static void input_check_resize(tc_term* t) {
    tc_input* in = t->input;
    int32_t   cols = 0;
    int32_t   rows = 0;

    if (!tc_signal_take_resize()) return;

    (void)tc_term_get_size(t, &cols, &rows);
    if (cols == in->last_resize_cols && rows == in->last_resize_rows)
        return;   /* same size again: merged */

    tc_event ev;
    int32_t  pw = 0;
    int32_t  ph = 0;
    memset(&ev, 0, sizeof(ev));
    ev.kind          = TC_EV_RESIZE;
    ev.seq           = in->seq++;
    ev.u.resize.cols = cols;
    ev.u.resize.rows = rows;
    (void)tc_term_get_size_px(t, &pw, &ph);
    ev.u.resize.pixel_w = pw;
    ev.u.resize.pixel_h = ph;

    (void)tc_input_enqueue(t, &ev);
    in->last_resize_cols = cols;
    in->last_resize_rows = rows;
}

/* Settles a pending residual: a lone ESC becomes TC_KEY_ESCAPE, any other
 * partial sequence is dropped, and the parser returns to GROUND (docs/04 §6,
 * §11). With forced=false this only fires once the esc_timeout_ms deadline
 * has passed; forced=true settles right now (tc_term_flush_pending). */
tc_status tc_input_settle(tc_term* t, bool forced, bool* produced) {
    tc_input* in = t->input;
    uint64_t  now;

    if (produced) *produced = false;
    if (in->residual_state == TC_IN_GROUND) return TC_OK;

    if (!forced) {
        now = tc_input_now_ms();
        if (now < in->residual_arrive_ms + (uint64_t)t->esc_timeout_ms)
            return TC_OK;   /* deadline not reached yet */
    }

    if (in->residual_state == TC_IN_ESC) {
        tc_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind       = TC_EV_KEY;
        ev.seq        = in->seq++;
        ev.u.key.key  = TC_KEY_ESCAPE;
        (void)tc_input_enqueue(t, &ev);
        if (produced) *produced = true;
    }
    /* Any other residual (partial CSI/SS3/OSC/DCS/UTF-8) is discarded. */

    in->state          = TC_IN_GROUND;
    in->seq_len        = 0;
    in->nparams        = 0;
    in->have_prefix    = false;
    in->prefix         = 0;
    in->utf8_len       = 0;
    in->utf8_expected  = 0;
    in->alt_pending    = false;
    in->residual_state = TC_IN_GROUND;
    return TC_OK;
}

tc_status tc_input_fill(tc_term* t) {
    tc_input* in = t->input;
    uint8_t   tmp[64];
    tc_status overflow = TC_OK;
    bool      produced = false;

    /* 1. Injected bytes go through the real parser. */
    while (in->inject_len > 0) {
        size_t chunk = in->inject_len > sizeof(tmp) ? sizeof(tmp) : in->inject_len;
        memcpy(tmp, in->inject_bytes, chunk);
        (void)tc_parser_feed(t, tmp, chunk);
        memmove(in->inject_bytes, in->inject_bytes + chunk,
                in->inject_len - chunk);
        in->inject_len -= chunk;
    }

    /* 2. Injected events bypass the parser, straight into the queue. */
    while (in->inject_e_count > 0) {
        tc_event ev = in->inject_events[in->inject_e_head];
        in->inject_e_head = (in->inject_e_head + 1) % TC_IN_INJECT_EVENTS;
        in->inject_e_count--;
        if (tc_input_enqueue(t, &ev) == TC_ERR_OVERFLOW && overflow == TC_OK)
            overflow = TC_ERR_OVERFLOW;
    }

    /* 3. Greedy read of the active source (external or default terminal). */
    input_greedy_read(t);

    /* 4. Resize events are generated lazily on the pull path. */
    input_check_resize(t);

    /* 5. Settle the ESC deadline if it has passed. */
    (void)tc_input_settle(t, false, &produced);

    return overflow;
}

/* --------------------------------------------------------------------------
 * Pull primitives (docs/04 §4)
 * ------------------------------------------------------------------------ */

tc_status tc_wait_event(tc_term_t* t, int32_t timeout_ms, bool* ready) {
    tc_input*  in;
    tc_status  st;
    bool       waited = false;

    if (!t || !ready) return TC_ERR_INVALID_ARG;
    *ready = false;
    if (t->state != TC_TERM_STATE_ACTIVE) return TC_ERR_STATE;
    in = t->input;

    /* Already something to deliver (queue, injections)? Then no waiting. */
    if (in->count == 0 && in->inject_len == 0 && in->inject_e_count == 0) {
        int32_t eff = timeout_ms;

        /* A pending residual bounds the wait: never sleep past its deadline
         * (docs/04 §11). */
        if (in->residual_state != TC_IN_GROUND && timeout_ms > 0) {
            uint64_t now = tc_input_now_ms();
            uint64_t dl  = in->residual_arrive_ms + (uint64_t)t->esc_timeout_ms;
            if (now >= dl) {
                eff = 0;
            } else {
                uint64_t remain = dl - now;
                if ((uint64_t)eff > remain) eff = (int32_t)remain;
            }
        }

        if (in->src_vt) {
            if (in->src_vt->wait_ready)
                (void)in->src_vt->wait_ready(in->src_ctx, eff, &waited);
        } else {
            (void)tc_backend_wait_ready(t->backend, eff, &waited);
        }
    }

    st = tc_input_fill(t);
    *ready = in->count > 0;
    return st;   /* TC_ERR_OVERFLOW only when DROP_NEWEST discarded an event */
}

static tc_status pull_event(tc_term* t, tc_event* out, bool take, bool* has) {
    tc_input* in;
    tc_status st;

    if (!t || !out || !has) return TC_ERR_INVALID_ARG;
    *has = false;
    if (t->state != TC_TERM_STATE_ACTIVE) return TC_ERR_STATE;
    in = t->input;

    st = tc_input_fill(t);

    if (in->count == 0) return st;
    *out = in->events[in->head];
    if (take) {
        in->head = (in->head + 1) % in->cap;
        in->count--;
    }
    *has = true;
    return st;   /* overflow reported, but the event was still delivered */
}

tc_status tc_peek_event(tc_term_t* t, tc_event* out, bool* has) {
    return pull_event(t, out, false, has);
}

tc_status tc_get_event(tc_term_t* t, tc_event* out, bool* has) {
    return pull_event(t, out, true, has);
}

tc_status tc_term_flush_pending(tc_term_t* t, bool* has) {
    if (!t || !has) return TC_ERR_INVALID_ARG;
    *has = false;
    if (t->state != TC_TERM_STATE_ACTIVE) return TC_ERR_STATE;
    return tc_input_settle(t, true, has);
}

/* --------------------------------------------------------------------------
 * Injection and input-source takeover (docs/04 §5)
 * ------------------------------------------------------------------------ */

tc_status tc_term_inject_input(tc_term_t* t, const void* bytes, size_t len) {
    tc_input* in;

    if (!t || (!bytes && len > 0)) return TC_ERR_INVALID_ARG;
    if (t->state != TC_TERM_STATE_ACTIVE) return TC_ERR_STATE;
    in = t->input;

    if (in->inject_len + len > TC_IN_INJECT_CAP) return TC_ERR_OVERFLOW;
    memcpy(in->inject_bytes + in->inject_len, bytes, len);
    in->inject_len += len;
    return TC_OK;
}

tc_status tc_term_inject_event(tc_term_t* t, const tc_event* ev) {
    tc_input* in;
    int32_t   slot;

    if (!t || !ev) return TC_ERR_INVALID_ARG;
    if (t->state != TC_TERM_STATE_ACTIVE) return TC_ERR_STATE;
    if (ev->kind < TC_EV_KEY || ev->kind > TC_EV_QUIT) return TC_ERR_INVALID_ARG;
    in = t->input;

    if (in->inject_e_count >= TC_IN_INJECT_EVENTS) return TC_ERR_OVERFLOW;
    slot = (in->inject_e_head + in->inject_e_count) % TC_IN_INJECT_EVENTS;
    in->inject_events[slot] = *ev;
    in->inject_e_count++;
    return TC_OK;
}

tc_status tc_term_set_input_source(tc_term_t* t, const tc_input_source_vtable* vt,
                                   void* ctx) {
    tc_input* in;

    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    in = t->input;

    if (in->src_vt && in->src_vt->dispose) in->src_vt->dispose(in->src_ctx);
    in->src_vt  = vt;
    in->src_ctx = ctx;
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * Diagnostics (docs/04 §16)
 * ------------------------------------------------------------------------ */

tc_status tc_key_name(tc_key k, const char** out) {
    static const char* const names[TC_KEY_COUNT] = {
        "NONE",
        "ENTER", "TAB", "BACKSPACE", "ESCAPE", "DELETE", "INSERT",
        "HOME", "END", "PAGE_UP", "PAGE_DOWN",
        "UP", "DOWN", "LEFT", "RIGHT",
        "F1", "F2", "F3", "F4", "F5", "F6",
        "F7", "F8", "F9", "F10", "F11", "F12",
        "KP_0", "KP_1", "KP_2", "KP_3", "KP_4",
        "KP_5", "KP_6", "KP_7", "KP_8", "KP_9", "KP_ENTER",
        "SHIFT", "CTRL", "ALT", "SUPER"
    };

    if (!out) return TC_ERR_INVALID_ARG;
    *out = NULL;
    if (k <= TC_KEY_NONE || k >= TC_KEY_COUNT) return TC_ERR_INVALID_ARG;
    *out = names[k];
    return TC_OK;
}

tc_status tc_mods_name(uint16_t mods, char* buf, size_t cap) {
    /* Human-readable order (ctrl+alt+shift ...), not bit order. */
    static const struct { uint16_t bit; const char* name; } kBits[] = {
        { TC_MOD_CTRL,  "CTRL"  }, { TC_MOD_ALT, "ALT" },
        { TC_MOD_SHIFT, "SHIFT" }, { TC_MOD_SUPER, "SUPER" },
        { TC_MOD_HYPER, "HYPER" }, { TC_MOD_META,  "META"  },
        { TC_MOD_CAPS,  "CAPS"  }, { TC_MOD_NUM,   "NUM"   }
    };
    size_t n = 0;
    size_t i;

    if (!buf || cap == 0) return TC_ERR_INVALID_ARG;
    buf[0] = '\0';

    for (i = 0; i < sizeof(kBits) / sizeof(kBits[0]); i++) {
        size_t l;
        if (!(mods & kBits[i].bit)) continue;
        l = strlen(kBits[i].name);
        if (n > 0) {
            if (n + 1 >= cap) return TC_ERR_OVERFLOW;
            buf[n++] = '+';
        }
        if (n + l >= cap) return TC_ERR_OVERFLOW;
        memcpy(buf + n, kBits[i].name, l);
        n += l;
    }
    buf[n] = '\0';
    return TC_OK;
}
