#include <control/term_internal.h>

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Runtime features (docs/02 §4).
 *
 * The restore path must work from an `atexit` hook and from a crash path,
 * where allocating is not an option (docs/02 §11), so every sequence here is
 * a compile-time constant shared from term_internal.h.
 *
 * Only features that were actually applied are undone, so `leave` never emits
 * noise bytes for something that never took effect.
 * ------------------------------------------------------------------------ */
/* DECSCUSR (shape) is not touched by enter/leave here: the session only ever
 * hides the cursor with DECTCEM, and the shape restore (TC_SEQ_CURSOR_SHAPE_
 * DEFAULT) is handled in restore_inner below via cursor_shape_applied
 * (docs/02 §7). */

static tc_status term_write(tc_term* t, const char* seq) {
    if (!t || !t->backend) return TC_ERR_STATE;
    return tc_backend_write(t->backend, seq, strlen(seq), NULL);
}

const char* tc_mouse_sequence(tc_mouse_mode mode, bool on) {
    switch (mode) {
    case TC_MOUSE_CLICK:  return on ? TC_SEQ_MOUSE_CLICK_ON  : TC_SEQ_MOUSE_CLICK_OFF;
    case TC_MOUSE_DRAG:   return on ? TC_SEQ_MOUSE_DRAG_ON   : TC_SEQ_MOUSE_DRAG_OFF;
    case TC_MOUSE_MOTION: return on ? TC_SEQ_MOUSE_MOTION_ON : TC_SEQ_MOUSE_MOTION_OFF;
    case TC_MOUSE_OFF:    /* fallthrough */
    default:              return NULL;
    }
}

static void term_note(tc_status* first, tc_status st) {
    if (*first == TC_OK) *first = st;
}

tc_status tc_term_apply_features(tc_term* t) {
    tc_status st;

    /* CTRL_C capture has no sequence of its own: it is a property of raw mode
     * plus the input layer's decoding (docs/02 §9). */
    t->applied[TC_FEATURE_CAPTURE_CTRL_C] = t->requested[TC_FEATURE_CAPTURE_CTRL_C];

    if (t->requested[TC_FEATURE_RAW_MODE]) {
        st = tc_backend_set_raw(t->backend, true);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_RAW_MODE] = true;
    }

    if (t->requested[TC_FEATURE_ALT_SCREEN]) {
        st = term_write(t, TC_SEQ_ALT_SCREEN_ON);
        if (st != TC_OK) return st;

        /* Undoable from here on: if the erase below fails, enter() runs
         * restore_features() and must be able to switch back (docs/02 §11). */
        t->applied[TC_FEATURE_ALT_SCREEN] = true;

        st = term_write(t, TC_SEQ_ALT_SCREEN_CLEAR);
        if (st != TC_OK) return st;
    }

    if (t->requested[TC_FEATURE_MOUSE] && t->mouse_mode != TC_MOUSE_OFF) {
        st = term_write(t, tc_mouse_sequence(t->mouse_mode, true));
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_MOUSE] = true;
        t->applied_mouse_mode        = t->mouse_mode;
    }

    if (t->requested[TC_FEATURE_FOCUS_EVENTS]) {
        st = term_write(t, TC_SEQ_FOCUS_ON);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_FOCUS_EVENTS] = true;
    }

    if (t->requested[TC_FEATURE_BRACKETED_PASTE]) {
        st = term_write(t, TC_SEQ_PASTE_ON);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_BRACKETED_PASTE] = true;
    }

    if (t->requested[TC_FEATURE_KITTY_KEYBOARD]) {
        st = term_write(t, TC_SEQ_KITTY_ON);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_KITTY_KEYBOARD] = true;
    }

    if (t->requested[TC_FEATURE_SYNC_UPDATE]) {
        st = term_write(t, TC_SEQ_SYNC_ON);
        if (st != TC_OK) return st;
        t->applied[TC_FEATURE_SYNC_UPDATE] = true;
    }

    if (t->hide_cursor) {
        st = term_write(t, TC_SEQ_CURSOR_HIDE);
        if (st != TC_OK) return st;
        t->cursor_hidden = true;
    }

    /* A shape requested before enter() is applied here (docs/02 §4.2). */
    if (t->cursor_shape != TC_CURSOR_DEFAULT) {
        char seq[16];
        snprintf(seq, sizeof(seq), "\x1b[%u q",
                 (unsigned)TC_DECSCUSR_PS(t->cursor_shape));
        st = term_write(t, seq);
        if (st != TC_OK) return st;
        t->cursor_shape_applied = true;
    }

    return TC_OK;
}

/* Reverse order, best effort: every step runs even if an earlier one failed,
 * and the first error seen is reported (docs/02 §11).
 *
 * With writeback=true this is the public leave() path: applied[] / state are
 * reset so a second leave is a no-op. With writeback=false it is the
 * async-signal-safe restore (docs/02 §11): the sequences are written and raw
 * mode is restored, but the state machine is left untouched so a signal that
 * lands mid-enter() cannot corrupt it (docs/02 §9). */
static tc_status restore_inner(tc_term* t, bool writeback) {
    tc_status first = TC_OK;

    if (t->applied[TC_FEATURE_SYNC_UPDATE]) {
        term_note(&first, term_write(t, TC_SEQ_SYNC_OFF));
        if (writeback) t->applied[TC_FEATURE_SYNC_UPDATE] = false;
    }

    if (t->applied[TC_FEATURE_KITTY_KEYBOARD]) {
        term_note(&first, term_write(t, TC_SEQ_KITTY_OFF));
        if (writeback) t->applied[TC_FEATURE_KITTY_KEYBOARD] = false;
    }

    if (t->applied[TC_FEATURE_MOUSE]) {
        term_note(&first, term_write(t, tc_mouse_sequence(t->applied_mouse_mode, false)));
        if (writeback) {
            t->applied[TC_FEATURE_MOUSE] = false;
            t->applied_mouse_mode        = TC_MOUSE_OFF;
        }
    }

    if (t->applied[TC_FEATURE_FOCUS_EVENTS]) {
        term_note(&first, term_write(t, TC_SEQ_FOCUS_OFF));
        if (writeback) t->applied[TC_FEATURE_FOCUS_EVENTS] = false;
    }

    if (t->applied[TC_FEATURE_BRACKETED_PASTE]) {
        term_note(&first, term_write(t, TC_SEQ_PASTE_OFF));
        if (writeback) t->applied[TC_FEATURE_BRACKETED_PASTE] = false;
    }

    if (t->applied[TC_FEATURE_ALT_SCREEN]) {
        /* 1049l restores the primary buffer *and* the cursor saved by 1049h, so
         * no further sequence may touch the screen here — no erase, no cursor
         * home — or the prompt underneath would not come back untouched. */
        term_note(&first, term_write(t, TC_SEQ_ALT_SCREEN_OFF));
        if (writeback) t->applied[TC_FEATURE_ALT_SCREEN] = false;
    }

    if (t->cursor_hidden) {
        term_note(&first, term_write(t, TC_SEQ_CURSOR_SHOW));
        if (writeback) t->cursor_hidden = false;
    }

    if (t->cursor_shape_applied) {
        /* A shape the session changed is restored to the terminal default;
         * a shape the user chose before enter() is left alone (docs/02 §7). */
        term_note(&first, term_write(t, TC_SEQ_CURSOR_SHAPE_DEFAULT));
        if (writeback) t->cursor_shape_applied = false;
    }

    if (t->applied[TC_FEATURE_RAW_MODE]) {
        term_note(&first, tc_backend_set_raw(t->backend, false));
        if (writeback) t->applied[TC_FEATURE_RAW_MODE] = false;
    }

    if (writeback) t->applied[TC_FEATURE_CAPTURE_CTRL_C] = false;

    return first;
}

tc_status tc_term_restore_features(tc_term* t) {
    return restore_inner(t, true);
}

tc_status tc_term_restore_signal(tc_term* t) {
    return restore_inner(t, false);
}
