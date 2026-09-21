#include <control/term_internal.h>

#include <caps/caps_internal.h>

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Runtime feature toggles (docs/02 §4���§7, §10, §12).
 *
 * Two state sets live on the term: `requested[]` (what the application asked
 * for, survives leave()) and `applied[]` (what was actually written). A setter
 * in CREATED state only touches requested[], and enter() applies everything in
 * one pass (feature.c); in ACTIVE state the setter writes the sequence and
 * updates applied[] immediately (docs/02 §4.2).
 *
 * Every toggle is idempotent: setting the current value writes nothing.
 * ------------------------------------------------------------------------ */

static tc_status term_write_seq(tc_term* t, const char* seq) {
    if (!t || !t->backend) return TC_ERR_STATE;
    return tc_backend_write(t->backend, seq, strlen(seq), NULL);
}

/* Applies one feature on an ACTIVE session: writes the on-sequence and marks
 * it applied. */
tc_status tc_feature_apply_one(tc_term* t, tc_feature f) {
    tc_status st;

    switch (f) {
    case TC_FEATURE_RAW_MODE:
        st = tc_backend_set_raw(t->backend, true);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_RAW_MODE] = true;
        return TC_OK;

    case TC_FEATURE_ALT_SCREEN:
        st = term_write_seq(t, TC_SEQ_ALT_SCREEN_ON);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_ALT_SCREEN] = true;
        /* Undoable from here on (mirrors apply_features). */
        return term_write_seq(t, TC_SEQ_ALT_SCREEN_CLEAR);

    case TC_FEATURE_MOUSE:
        /* A bare set_feature(MOUSE, true) has no mode; promote to CLICK. */
        if (t->mouse_mode == TC_MOUSE_OFF) t->mouse_mode = TC_MOUSE_CLICK;
        st = term_write_seq(t, tc_mouse_sequence(t->mouse_mode, true));
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_MOUSE] = true;
        t->applied_mouse_mode        = t->mouse_mode;
        return TC_OK;

    case TC_FEATURE_FOCUS_EVENTS:
        st = term_write_seq(t, TC_SEQ_FOCUS_ON);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_FOCUS_EVENTS] = true;
        return TC_OK;

    case TC_FEATURE_BRACKETED_PASTE:
        st = term_write_seq(t, TC_SEQ_PASTE_ON);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_BRACKETED_PASTE] = true;
        return TC_OK;

    case TC_FEATURE_KITTY_KEYBOARD:
        st = term_write_seq(t, TC_SEQ_KITTY_ON);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_KITTY_KEYBOARD] = true;
        return TC_OK;

    case TC_FEATURE_SYNC_UPDATE:
        st = term_write_seq(t, TC_SEQ_SYNC_ON);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_SYNC_UPDATE] = true;
        return TC_OK;

    case TC_FEATURE_CAPTURE_CTRL_C:
        /* No sequence of its own: a property of raw mode plus decoding. */
        t->applied[TC_FEATURE_CAPTURE_CTRL_C] = true;
        return TC_OK;

    default:
        return TC_ERR_INVALID_ARG;
    }
}

/* Undoes one feature on an ACTIVE session. */
tc_status tc_feature_undo_one(tc_term* t, tc_feature f) {
    tc_status st;

    switch (f) {
    case TC_FEATURE_RAW_MODE:
        st = tc_backend_set_raw(t->backend, false);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_RAW_MODE] = false;
        return TC_OK;

    case TC_FEATURE_ALT_SCREEN:
        st = term_write_seq(t, TC_SEQ_ALT_SCREEN_OFF);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_ALT_SCREEN] = false;
        return TC_OK;

    case TC_FEATURE_MOUSE:
        if (t->applied[TC_FEATURE_MOUSE]) {
            const char* off = tc_mouse_sequence(t->applied_mouse_mode, false);
            if (off) {
                st = term_write_seq(t, off);
                if (st != TC_OK) return st;
            }
            t->applied[TC_FEATURE_MOUSE] = false;
            t->applied_mouse_mode        = TC_MOUSE_OFF;
        }
        return TC_OK;

    case TC_FEATURE_FOCUS_EVENTS:
        if (t->applied[TC_FEATURE_FOCUS_EVENTS]) {
            st = term_write_seq(t, TC_SEQ_FOCUS_OFF);
            if (st != TC_OK) return st;
            t->applied[TC_FEATURE_FOCUS_EVENTS] = false;
        }
        return TC_OK;

    case TC_FEATURE_BRACKETED_PASTE:
        if (t->applied[TC_FEATURE_BRACKETED_PASTE]) {
            st = term_write_seq(t, TC_SEQ_PASTE_OFF);
            if (st != TC_OK) return st;
            t->applied[TC_FEATURE_BRACKETED_PASTE] = false;
        }
        return TC_OK;

    case TC_FEATURE_KITTY_KEYBOARD:
        if (t->applied[TC_FEATURE_KITTY_KEYBOARD]) {
            st = term_write_seq(t, TC_SEQ_KITTY_OFF);
            if (st != TC_OK) return st;
            t->applied[TC_FEATURE_KITTY_KEYBOARD] = false;
        }
        return TC_OK;

    case TC_FEATURE_SYNC_UPDATE:
        if (t->applied[TC_FEATURE_SYNC_UPDATE]) {
            st = term_write_seq(t, TC_SEQ_SYNC_OFF);
            if (st != TC_OK) return st;
            t->applied[TC_FEATURE_SYNC_UPDATE] = false;
        }
        return TC_OK;

    case TC_FEATURE_CAPTURE_CTRL_C:
        t->applied[TC_FEATURE_CAPTURE_CTRL_C] = false;
        return TC_OK;

    default:
        return TC_ERR_INVALID_ARG;
    }
}

/* --------------------------------------------------------------------------
 * Feature API (docs/02 §4)
 * ------------------------------------------------------------------------ */

tc_status tc_term_set_feature(tc_term_t* t, tc_feature f, bool on) {
    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    if ((unsigned)f >= (unsigned)TC_FEATURE_COUNT) return TC_ERR_INVALID_ARG;

    if (on == t->requested[f]) return TC_OK;   /* idempotent */

    t->requested[f] = on;
    if (t->state != TC_TERM_STATE_ACTIVE) return TC_OK;   /* enter applies it */

    return on ? tc_feature_apply_one(t, f) : tc_feature_undo_one(t, f);
}

tc_status tc_term_get_feature(const tc_term_t* t, tc_feature f,
                              bool* requested, bool* effective) {
    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    if ((unsigned)f >= (unsigned)TC_FEATURE_COUNT) return TC_ERR_INVALID_ARG;

    if (requested) *requested = t->requested[f];
    /* effective = requested && caps allow (docs/02 ?4, docs/03 ?10.2): the
     * capability bits narrow it. The mouse mode is degraded through the caps
     * chain (docs/03 ?7). */
    if (effective) {
        *effective = t->requested[f];
        if (*effective && t->caps) {
            if (f == TC_FEATURE_MOUSE) {
                *effective = tc_caps_effective_mouse(&t->caps->caps, t->mouse_mode)
                             != TC_MOUSE_OFF;
            } else {
                *effective = tc_caps_allows_feature(&t->caps->caps, f);
            }
        }
    }
    return TC_OK;
}

tc_status tc_term_set_mouse_mode(tc_term_t* t, tc_mouse_mode mode) {
    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    if ((unsigned)mode > (unsigned)TC_MOUSE_MOTION) return TC_ERR_INVALID_ARG;

    if (mode == t->mouse_mode) return TC_OK;   /* idempotent */

    /* Undo whatever mouse state is active before switching modes. */
    if (t->state == TC_TERM_STATE_ACTIVE && t->applied[TC_FEATURE_MOUSE]) {
        const char* off = tc_mouse_sequence(t->applied_mouse_mode, false);
        if (off) {
            tc_status st = term_write_seq(t, off);
            if (st != TC_OK) return st;
        }
        t->applied[TC_FEATURE_MOUSE] = false;
        t->applied_mouse_mode        = TC_MOUSE_OFF;
    }

    t->mouse_mode                   = mode;
    t->requested[TC_FEATURE_MOUSE]  = (mode != TC_MOUSE_OFF);

    if (t->state == TC_TERM_STATE_ACTIVE && mode != TC_MOUSE_OFF) {
        tc_status st = term_write_seq(t, tc_mouse_sequence(mode, true));
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_MOUSE] = true;
        t->applied_mouse_mode        = mode;
    }
    return TC_OK;
}

tc_status tc_term_get_mouse_mode(const tc_term_t* t, tc_mouse_mode* mode) {
    if (!t || !mode) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    *mode = t->mouse_mode;
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * Raw mode and alternate screen (docs/02 §5, §6): convenience entries that
 * share the same state as the feature API.
 * ------------------------------------------------------------------------ */

tc_status tc_term_set_raw(tc_term_t* t, bool on) {
    return tc_term_set_feature(t, TC_FEATURE_RAW_MODE, on);
}

tc_status tc_term_is_raw(const tc_term_t* t, bool* on) {
    if (!t || !on) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    *on = t->requested[TC_FEATURE_RAW_MODE];
    return TC_OK;
}

tc_status tc_term_enter_alt_screen(tc_term_t* t) {
    return tc_term_set_feature(t, TC_FEATURE_ALT_SCREEN, true);
}

tc_status tc_term_leave_alt_screen(tc_term_t* t) {
    return tc_term_set_feature(t, TC_FEATURE_ALT_SCREEN, false);
}

/* --------------------------------------------------------------------------
 * Cursor (docs/02 §7)
 * ------------------------------------------------------------------------ */

tc_status tc_term_set_cursor_visible(tc_term_t* t, bool visible) {
    tc_status st;

    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    if (t->hide_cursor == !visible) return TC_OK;   /* idempotent */

    t->hide_cursor = !visible;
    if (t->state != TC_TERM_STATE_ACTIVE) return TC_OK;

    if (visible) {
        st = term_write_seq(t, TC_SEQ_CURSOR_SHOW);
        if (st != TC_OK) return st;
        t->cursor_hidden = false;
    } else {
        st = term_write_seq(t, TC_SEQ_CURSOR_HIDE);
        if (st != TC_OK) return st;
        t->cursor_hidden = true;
    }
    return TC_OK;
}

tc_status tc_term_set_cursor_shape(tc_term_t* t, tc_cursor_shape shape) {
    char     seq[16];
    int      n;
    tc_status st;

    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    if ((unsigned)shape > (unsigned)TC_CURSOR_BLINK_BAR) return TC_ERR_INVALID_ARG;

    if (shape == t->cursor_shape) return TC_OK;   /* idempotent */

    t->cursor_shape = shape;
    if (t->state != TC_TERM_STATE_ACTIVE) return TC_OK;   /* enter applies it */

    n = snprintf(seq, sizeof(seq), "\x1b[%u q",
                 (unsigned)TC_DECSCUSR_PS(shape));
    if (n <= 0 || (size_t)n >= sizeof(seq)) return TC_ERR_OVERFLOW;
    st = term_write_seq(t, seq);
    if (st != TC_OK) return st;

    t->cursor_shape_applied = (shape != TC_CURSOR_DEFAULT);
    return TC_OK;
}

tc_status tc_term_set_cursor_pos(tc_term_t* t, int32_t x, int32_t y) {
    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    if (x < 0 || y < 0) return TC_ERR_INVALID_ARG;

    /* A position is transient, not a requested state: nothing to record in
     * CREATED state, and nothing to re-apply on enter(). */
    if (t->state != TC_TERM_STATE_ACTIVE) return TC_OK;
    return tc_backend_set_cursor_pos(t->backend, x, y);
}

/* --------------------------------------------------------------------------
 * Title (docs/02 §10)
 * ------------------------------------------------------------------------ */

tc_status tc_term_set_title(tc_term_t* t, const char* utf8) {
    if (!t || !utf8) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    return tc_backend_set_title(t->backend, utf8);
}

/* --------------------------------------------------------------------------
 * Text mode (docs/02 §12)
 * ------------------------------------------------------------------------ */

tc_status tc_term_set_text_mode(tc_term_t* t, tc_text_mode mode) {
    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    if ((unsigned)mode > (unsigned)TC_TEXT_UNICODE) return TC_ERR_INVALID_ARG;
    t->text_mode = mode;
    return TC_OK;
}

tc_status tc_term_get_text_mode(const tc_term_t* t, tc_text_mode* mode) {
    if (!t || !mode) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    *mode = t->text_mode;
    return TC_OK;
}

tc_status tc_term_set_ambiguous_wide(tc_term_t* t, bool wide) {
    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    t->ambiguous_wide = wide;
    return TC_OK;
}
