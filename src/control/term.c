#include <termcore/tc_term.h>

#include <control/signal_internal.h>
#include <control/term_internal.h>
#include <platform/backend.h>

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Control layer: session lifecycle (docs/02 §2, §3, §11).
 * ------------------------------------------------------------------------ */
#define TC_DEFAULT_COLS 80
#define TC_DEFAULT_ROWS 24
#define TC_MAX_SIZE     4096

/* Every live session, newest first. Intrusive, so registering never allocates
 * and the atexit hook can walk it without touching the heap. */
static tc_term* g_live_terms       = NULL;
static bool     g_atexit_installed = false;
static unsigned g_live_count       = 0;   /* first create installs signal hooks, last destroy removes them */

static void tc_term_atexit(void) {
    /* Best effort: no allocation, no error propagation (docs/02 §11). */
    tc_term* t = g_live_terms;
    while (t) {
        if (t->state == TC_TERM_STATE_ACTIVE) (void)tc_term_leave(t);
        t = t->next_live;
    }
}

static void term_register(tc_term* t) {
    if (!g_atexit_installed) {
        g_atexit_installed = true;
        atexit(tc_term_atexit);
    }
    if (g_live_count == 0) (void)tc_signal_install();   /* docs/02 §9 */
    t->next_live   = g_live_terms;
    g_live_terms   = t;
    g_live_count++;
}

static void term_unregister(tc_term* t) {
    tc_term** link = &g_live_terms;
    while (*link) {
        if (*link == t) {
            *link        = t->next_live;
            t->next_live = NULL;
            break;
        }
        link = &(*link)->next_live;
    }
    if (g_live_count > 0) g_live_count--;
    if (g_live_count == 0) tc_signal_uninstall();
}

/* Read-only walk for the signal module (signal_internal.h). */
const tc_term* tc_term_live_first(void) {
    return g_live_terms;
}

const tc_term* tc_term_live_next(const tc_term* t) {
    return t ? t->next_live : NULL;
}

/* --------------------------------------------------------------------------
 * Options
 * ------------------------------------------------------------------------ */
void tc_init_term_options(tc_term_options* opt) {
    if (!opt) return;

    memset(opt, 0, sizeof(*opt));

    /* runtime features */
    opt->raw_mode         = true;
    opt->alternate_screen = true;
    opt->mouse_mode       = TC_MOUSE_OFF;
    opt->focus_events     = false;
    opt->bracketed_paste  = true;
    opt->kitty_keyboard   = true;
    opt->hide_cursor      = true;
    opt->capture_ctrl_c   = true;
    opt->sync_update      = true;

    /* init-only attributes */
    opt->headless           = false;
    opt->query_capabilities = true;
    opt->caps_profile       = NULL;
    opt->strict_diff        = false;
    opt->text_mode          = TC_TEXT_UNICODE;
    opt->ambiguous_wide     = false;

    /* time and capacity */
    opt->capability_timeout_ms = 200;
    opt->esc_timeout_ms        = 100;
    opt->read_batch_max_bytes  = 4096;
    opt->event_queue_capacity  = 256;
    opt->overflow              = TC_OVERFLOW_DROP_OLDEST;

    opt->allocator = NULL;
}

static void term_load_options(tc_term* t, const tc_term_options* opt) {
    /* init-only attributes: frozen here, never re-read (docs/02 §4) */
    t->headless              = opt->headless;
    t->query_capabilities    = opt->query_capabilities && !opt->headless;
    t->strict_diff           = opt->strict_diff;
    t->text_mode             = opt->text_mode;
    t->ambiguous_wide        = opt->ambiguous_wide;
    t->capability_timeout_ms = opt->capability_timeout_ms;
    t->esc_timeout_ms        = opt->esc_timeout_ms;
    t->read_batch_max_bytes =
        opt->read_batch_max_bytes > 0 ? opt->read_batch_max_bytes : 4096;
    t->event_queue_capacity =
        opt->event_queue_capacity > 0 ? opt->event_queue_capacity : 256;
    t->overflow              = opt->overflow;

    /* runtime features: initial values, changeable later (docs/02 §4) */
    t->requested[TC_FEATURE_RAW_MODE]         = opt->raw_mode;
    t->requested[TC_FEATURE_ALT_SCREEN]       = opt->alternate_screen;
    t->requested[TC_FEATURE_MOUSE]            = opt->mouse_mode != TC_MOUSE_OFF;
    t->requested[TC_FEATURE_FOCUS_EVENTS]     = opt->focus_events;
    t->requested[TC_FEATURE_BRACKETED_PASTE]  = opt->bracketed_paste;
    t->requested[TC_FEATURE_KITTY_KEYBOARD]   = opt->kitty_keyboard;
    t->requested[TC_FEATURE_SYNC_UPDATE]      = opt->sync_update;
    t->requested[TC_FEATURE_CAPTURE_CTRL_C]   = opt->capture_ctrl_c;
    t->mouse_mode                             = opt->mouse_mode;
    t->hide_cursor                            = opt->hide_cursor;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */
tc_status tc_term_create(const tc_term_options* opt, tc_term_t** out) {
    tc_term_options     defaults;
    const tc_allocator* alloc;
    tc_backend_kind     kind;
    tc_term*            t;
    tc_status           st;
    int32_t             cols = 0;
    int32_t             rows = 0;

    if (!out) return TC_ERR_INVALID_ARG;
    *out = NULL;

    if (!opt) {
        tc_init_term_options(&defaults);
        opt = &defaults;
    }

    if (!opt->headless && !tc_stdout_is_tty()) return TC_ERR_NOT_A_TTY;

    alloc = opt->allocator ? opt->allocator : tc_allocator_default();

    t = (tc_term*)alloc->alloc(alloc->ctx, sizeof(*t), sizeof(void*));
    if (!t) return TC_ERR_NOMEM;

    memset(t, 0, sizeof(*t));
    t->alloc        = alloc;
    t->state        = TC_TERM_STATE_CREATED;
    t->mouse_mode   = TC_MOUSE_OFF;
    t->applied_mouse_mode = TC_MOUSE_OFF;

    term_load_options(t, opt);

    kind = t->headless ? TC_BACKEND_NULL : tc_platform_default_backend();
    st   = tc_backend_create(kind, alloc, &t->backend);
    if (st != TC_OK) {
        alloc->free(alloc->ctx, t, sizeof(*t));
        return st;
    }
    t->backend_kind = (tc_backend_kind)t->backend->kind;

    /* Seed the size: a fresh session must answer tc_term_get_size even before
     * the first enter(). */
    if (tc_backend_get_size(t->backend, &cols, &rows) != TC_OK || cols <= 0 || rows <= 0) {
        cols = TC_DEFAULT_COLS;
        rows = TC_DEFAULT_ROWS;
    }
    t->cols = cols;
    t->rows = rows;

    /* TODO(caps): with opt->caps_profile / query_capabilities, probe or adopt
     * the capability bits here — the capability layer (docs/03) owns it. */

    term_register(t);

    *out = (tc_term_t*)t;
    return TC_OK;
}

tc_status tc_term_enter(tc_term_t* t) {
    tc_status st;

    if (!t) return TC_ERR_INVALID_ARG;

    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    if (t->state == TC_TERM_STATE_ACTIVE)    return TC_ERR_STATE;

    st = tc_term_apply_features(t);
    if (st != TC_OK) {
        /* Do not leave the terminal half-way through a transition. */
        (void)tc_term_restore_features(t);
        return st;
    }

    t->state = TC_TERM_STATE_ACTIVE;
    return TC_OK;
}

tc_status tc_term_leave(tc_term_t* t) {
    tc_status st;

    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    if (t->state != TC_TERM_STATE_ACTIVE)    return TC_OK;   /* idempotent */

    st = tc_term_restore_features(t);

    /* Requested state survives leave(): a second enter() re-applies it
     * (docs/02 §4.2). */
    t->state = TC_TERM_STATE_CREATED;
    return st;
}

/* Re-enters a session from normal control flow after a suspend/resume cycle.
 *
 * The SIGTSTP handler restores the terminal without touching the state machine
 * (docs/02 §9): the session is still ACTIVE while the terminal is physically
 * back to normal. On SIGCONT the application polls tc_signal_take_resume()
 * and calls this: leave() first clears applied[] (the terminal was already
 * restored, so the off-sequences are idempotent noise), then enter() re-applies
 * every requested feature. */
tc_status tc_term_reenter(tc_term_t* t) {
    tc_status st;

    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    if (t->state == TC_TERM_STATE_ACTIVE) {
        st = tc_term_leave(t);
        if (st != TC_OK) return st;
    }
    return tc_term_enter(t);
}

void tc_term_destroy(tc_term_t* t) {
    const tc_allocator* alloc;

    if (!t) return;

    (void)tc_term_leave(t);

    term_unregister(t);

    if (t->backend) {
        tc_backend_destroy(t->backend);
        t->backend = NULL;
    }

    alloc = t->alloc ? t->alloc : tc_allocator_default();
    t->state = TC_TERM_STATE_DESTROYED;
    alloc->free(alloc->ctx, t, sizeof(*t));
}

tc_status tc_term_is_active(const tc_term_t* t, bool* active) {
    if (!t || !active) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    *active = (t->state == TC_TERM_STATE_ACTIVE);
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * Size
 * ------------------------------------------------------------------------ */
tc_status tc_term_get_size(const tc_term_t* t, int32_t* cols, int32_t* rows) {
    tc_term* m;

    if (!t || (!cols && !rows)) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    /* Refreshing a cache is not a semantic change, hence the cast. */
    m = (tc_term*)t;

    if (!m->size_forced && m->backend) {
        int32_t c = 0;
        int32_t r = 0;
        if (tc_backend_get_size(m->backend, &c, &r) == TC_OK && c > 0 && r > 0) {
            m->cols = c;
            m->rows = r;
        }
    }

    if (cols) *cols = m->cols;
    if (rows) *rows = m->rows;
    return TC_OK;
}

tc_status tc_term_get_size_px(const tc_term_t* t, int32_t* w, int32_t* h) {
    tc_term* m;

    if (!t || (!w && !h)) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    m = (tc_term*)t;

    if (m->backend) {
        int32_t pw = 0;
        int32_t ph = 0;
        (void)tc_backend_get_size_px(m->backend, &pw, &ph);
        m->cols_px = pw > 0 ? pw : 0;
        m->rows_px = ph > 0 ? ph : 0;
    }

    if (w) *w = m->cols_px;
    if (h) *h = m->rows_px;
    return (m->cols_px > 0 && m->rows_px > 0) ? TC_OK : TC_ERR_UNSUPPORTED;
}

tc_status tc_term_set_size(tc_term_t* t, int32_t cols, int32_t rows) {
    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    /* Docs/02 §3: only the null (headless) backend accepts a forced size. */
    if (t->backend_kind != TC_BACKEND_NULL) return TC_ERR_UNSUPPORTED;
    if (cols <= 0 || rows <= 0 || cols > TC_MAX_SIZE || rows > TC_MAX_SIZE)
        return TC_ERR_INVALID_ARG;

    t->cols        = cols;
    t->rows        = rows;
    t->size_forced = true;
    return TC_OK;
}
