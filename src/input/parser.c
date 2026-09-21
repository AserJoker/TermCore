/* Input parser state machine (docs/04 §6, §7, §8, §9, §10).
 *
 * Byte stream -> events. The state machine is the only place that decides
 * what a byte means; it feeds produced events straight into the term's event
 * queue. Cross-batch input (a sequence split across two reads) is handled by
 * simply keeping the parser state between calls; the lone-ESC ambiguity is
 * settled by the caller (input.c) through the deadline mechanism (docs/04
 * §11), never by blocking here.
 *
 * Handler contract: every parser_* returns 0 when the byte was consumed and
 * 1 when it must be reprocessed (the parser moved back to GROUND first).
 * `ovf` accumulates the first queue overflow (DROP_NEWEST policy).
 */
#include <input/input_internal.h>

#include <termcore/tc_text.h>

#include <string.h>

/* --------------------------------------------------------------------------
 * Emit helpers (events leave the parser only through the term's queue)
 * ------------------------------------------------------------------------ */

static void acc(tc_status* ovf, tc_status r) {
    if (*ovf == TC_OK && r != TC_OK) *ovf = r;
}

static tc_status emit_key(tc_term* t, uint32_t cp, tc_key key, uint16_t mods,
                          bool release) {
    tc_input* in = t->input;
    tc_event  ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind           = TC_EV_KEY;
    ev.seq            = in->seq++;
    ev.u.key.codepoint = cp;
    ev.u.key.key      = key;
    ev.u.key.mods     = mods;
    ev.u.key.is_release = release;
    return tc_input_enqueue(t, &ev);
}

static tc_status emit_focus(tc_term* t, bool focused) {
    tc_input* in = t->input;
    tc_event  ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind           = TC_EV_FOCUS;
    ev.seq            = in->seq++;
    ev.u.focus.focused = focused;
    return tc_input_enqueue(t, &ev);
}

static tc_status emit_paste(tc_term* t, size_t len) {
    tc_input* in = t->input;
    tc_event  ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind        = TC_EV_PASTE;
    ev.seq         = in->seq++;
    ev.u.paste.text = in->paste_buf;   /* borrowed, single shared buffer */
    ev.u.paste.len = len;
    return tc_input_enqueue(t, &ev);
}

/* --------------------------------------------------------------------------
 * Modifier decoding (docs/04 §8)
 * ------------------------------------------------------------------------ */

/* xterm modifier parameter: 1 + Shift + 2*Alt + 4*Ctrl + 8*Meta (docs/04
 * §8.1). The base 1 is always present, so it is subtracted before the bits
 * are tested: Pm=2 -> Shift, Pm=3 -> Alt, Pm=5 -> Ctrl. */
static uint16_t xterm_mods(int32_t pm) {
    uint16_t m = 0;
    if (pm <= 0) return 0;
    pm -= 1;
    if (pm & 1)  m |= TC_MOD_SHIFT;
    if (pm & 2)  m |= TC_MOD_ALT;
    if (pm & 4)  m |= TC_MOD_CTRL;
    if (pm & 8)  m |= TC_MOD_META;
    return m;
}

/* Kitty bit-set: 1 Shift, 2 Alt, 4 Ctrl, 8 Super, 16 Hyper, 32 Meta,
 * 64 CapsLock, 128 NumLock (docs/04 §8.2). */
static uint16_t kitty_mods(int32_t v) {
    uint16_t m = 0;
    if (v < 0) return 0;
    if (v & 1)   m |= TC_MOD_SHIFT;
    if (v & 2)   m |= TC_MOD_ALT;
    if (v & 4)   m |= TC_MOD_CTRL;
    if (v & 8)   m |= TC_MOD_SUPER;
    if (v & 16)  m |= TC_MOD_HYPER;
    if (v & 32)  m |= TC_MOD_META;
    if (v & 64)  m |= TC_MOD_CAPS;
    if (v & 128) m |= TC_MOD_NUM;
    return m;
}

/* --------------------------------------------------------------------------
 * Mouse (docs/04 §9)
 * ------------------------------------------------------------------------ */

/* Mode gating: the sequence is always recognized (never misread as keys),
 * but DRAG / MOVE events only appear per the runtime mode. */
static bool mouse_action_allowed(const tc_term* t, tc_mouse_action action) {
    switch (t->mouse_mode) {
    case TC_MOUSE_OFF:
        return false;
    case TC_MOUSE_CLICK:
        return action == TC_MOUSE_ACT_DOWN || action == TC_MOUSE_ACT_UP ||
               action == TC_MOUSE_ACT_WHEEL_UP ||
               action == TC_MOUSE_ACT_WHEEL_DOWN;
    case TC_MOUSE_DRAG:
        return action != TC_MOUSE_ACT_MOVE;
    case TC_MOUSE_MOTION:
        return true;
    }
    return false;
}

/* Shared SGR + X10 payload interpretation. cb is the button byte, px/py the
 * 1-based cell coordinates; sgr_release marks the 'm' final (SGR only). */
static void mouse_common(tc_term* t, int32_t cb, int32_t px, int32_t py,
                         bool sgr_release, tc_status* ovf) {
    tc_input*         in = t->input;
    tc_mouse_button   btn;
    tc_mouse_action   action;
    uint16_t          mods = 0;
    bool              motion;
    int32_t           x = px - 1;   /* 1-based on the wire, 0-based here */
    int32_t           y = py - 1;
    tc_event          ev;

    if (cb < 0 || x < 0 || y < 0) return;   /* negative: drop (docs §9.2) */

    if (cb & 4)  mods |= TC_MOD_SHIFT;
    if (cb & 8)  mods |= TC_MOD_ALT;
    if (cb & 16) mods |= TC_MOD_CTRL;

    /* Clamp to the current size; do not drop oversized-but-valid coords. */
    if (t->cols > 0 && x >= t->cols) x = t->cols - 1;
    if (t->rows > 0 && y >= t->rows) y = t->rows - 1;

    if (cb & 64) {   /* wheel: 64 up / 65 down, no press-state change */
        btn    = TC_MOUSE_WHEEL;
        action = (cb & 1) ? TC_MOUSE_ACT_WHEEL_DOWN : TC_MOUSE_ACT_WHEEL_UP;
    } else {
        int32_t b = cb & 3;   /* wire code: 0 left, 1 middle, 2 right, 3 release */
        motion    = (cb & 32) != 0;
        if (b == 3 || sgr_release) {
            btn = in->pressed_valid ? in->pressed : TC_MOUSE_LEFT;
            action = motion ? TC_MOUSE_ACT_MOVE : TC_MOUSE_ACT_UP;
            in->pressed       = TC_MOUSE_NONE;
            in->pressed_valid = false;
        } else {
            /* Wire codes are 0-based; the public enum reserves 0 for none. */
            btn = (tc_mouse_button)(b + 1);
            if (motion) {
                action = (in->pressed_valid && in->pressed == btn)
                             ? TC_MOUSE_ACT_DRAG : TC_MOUSE_ACT_MOVE;
            } else {
                action = TC_MOUSE_ACT_DOWN;
                in->pressed       = btn;
                in->pressed_valid = true;
            }
        }
    }

    if (!mouse_action_allowed(t, action)) return;

    memset(&ev, 0, sizeof(ev));
    ev.kind           = TC_EV_MOUSE;
    ev.seq            = in->seq++;
    ev.u.mouse.x      = x;
    ev.u.mouse.y      = y;
    ev.u.mouse.button = btn;
    ev.u.mouse.action = action;
    ev.u.mouse.mods   = mods;
    acc(ovf, tc_input_enqueue(t, &ev));
}

/* --------------------------------------------------------------------------
 * CSI final-byte dispatch
 * ------------------------------------------------------------------------ */

static void csi_tilde(tc_term* t, tc_status* ovf) {
    tc_input* in = t->input;
    int32_t   code = in->params[0];
    uint16_t  mods = 0;
    tc_key    key  = TC_KEY_NONE;

    if (in->nparams >= 1) mods = xterm_mods(in->params[1]);

    switch (code) {
    case 1: case 7: key = TC_KEY_HOME;   break;
    case 2:          key = TC_KEY_INSERT; break;
    case 3:          key = TC_KEY_DELETE; break;
    case 4: case 8: key = TC_KEY_END;    break;
    case 5:          key = TC_KEY_PAGE_UP;   break;
    case 6:          key = TC_KEY_PAGE_DOWN; break;
    case 11: key = TC_KEY_F1;  break;
    case 12: key = TC_KEY_F2;  break;
    case 13: key = TC_KEY_F3;  break;
    case 14: key = TC_KEY_F4;  break;
    case 15: key = TC_KEY_F5;  break;
    case 17: key = TC_KEY_F6;  break;
    case 18: key = TC_KEY_F7;  break;
    case 19: key = TC_KEY_F8;  break;
    case 20: key = TC_KEY_F9;  break;
    case 21: key = TC_KEY_F10; break;
    case 23: key = TC_KEY_F11; break;
    case 24: key = TC_KEY_F12; break;

    case 27:   /* modifyOtherKeys: CSI 27 ; Pm ; code ~ (docs/04 §8.3) */
        if (in->nparams >= 2) {
            uint32_t cp = (uint32_t)(in->params[2] > 0 ? in->params[2] : 0);
            uint16_t m  = xterm_mods(in->params[1]);
            acc(ovf, emit_key(t, cp, TC_KEY_NONE, m, false));
        }
        return;

    case 200:  /* bracketed paste start (docs/04 §10) */
        if (t->requested[TC_FEATURE_BRACKETED_PASTE]) {
            in->state     = TC_IN_PASTE;
            in->paste_len = 0;
            in->paste_total = 0;
            memset(in->paste_tail, 0, sizeof(in->paste_tail));
        }
        return;   /* disabled: recognized but produces nothing */

    case 201:   /* stray paste end: consume */
        return;

    default:
        return;   /* unknown ~ key: drop the sequence */
    }

    acc(ovf, emit_key(t, 0, key, mods, false));
}

static void csi_kitty(tc_term* t, tc_status* ovf) {
    tc_input* in = t->input;
    uint32_t  cp  = (uint32_t)(in->params[0] > 0 ? in->params[0] : 0);
    uint16_t  mods = 0;
    int32_t   type = 1;   /* 1 press, 2 repeat, 3 release */
    int32_t   modpos = -1;
    int32_t   i;

    /* First ';'-separated param is the mods bit-set; fields before it (';'
     * or ':' separated) are shifted/base alternates we do not need. */
    for (i = 1; i <= in->nparams; i++) {
        if (in->sep[i] == ';') { modpos = i; break; }
    }
    if (modpos > 0) {
        mods = kitty_mods(in->params[modpos]);
        if (modpos + 1 <= in->nparams) {
            type = in->params[modpos + 1];
            if (type < 1 || type > 3) type = 1;
        }
    }
    acc(ovf, emit_key(t, cp, TC_KEY_NONE, mods, type == 3));
}

/* Returns 0 consumed, 1 reprocess. */
static int csi_final(tc_term* t, uint8_t c, tc_status* ovf) {
    tc_input* in = t->input;
    uint16_t  mods;
    tc_key    key;

    switch (c) {
    case 'A': case 'B': case 'C': case 'D':
        key = c == 'A' ? TC_KEY_UP : c == 'B' ? TC_KEY_DOWN
            : c == 'C' ? TC_KEY_RIGHT : TC_KEY_LEFT;
        mods = (in->nparams >= 1) ? xterm_mods(in->params[1]) : 0;
        acc(ovf, emit_key(t, 0, key, mods, false));
        return 0;

    case 'H': case 'F':
        key  = (c == 'H') ? TC_KEY_HOME : TC_KEY_END;
        mods = (in->nparams >= 1) ? xterm_mods(in->params[1]) : 0;
        acc(ovf, emit_key(t, 0, key, mods, false));
        return 0;

    case 'Z':   /* Shift+Tab */
        acc(ovf, emit_key(t, 0, TC_KEY_TAB, TC_MOD_SHIFT, false));
        return 0;

    case 'I':   /* focus in / out (docs/04 §10), gated by the runtime switch */
        if (t->requested[TC_FEATURE_FOCUS_EVENTS])
            acc(ovf, emit_focus(t, true));
        return 0;

    case 'O':
        if (t->requested[TC_FEATURE_FOCUS_EVENTS])
            acc(ovf, emit_focus(t, false));
        return 0;

    case 'M':   /* mouse: SGR with '<' prefix, X10 payload without */
        if (in->have_prefix && in->prefix == '<') {
            int32_t cb = in->params[0];
            int32_t px = in->nparams >= 1 ? in->params[1] : 0;
            int32_t py = in->nparams >= 2 ? in->params[2] : 0;
            mouse_common(t, cb, px, py, false, ovf);
        } else if (!in->have_prefix) {
            in->state  = TC_IN_X10;
            in->x10_len = 0;
        }
        return 0;

    case 'm':   /* SGR release */
        if (in->have_prefix && in->prefix == '<') {
            int32_t cb = in->params[0];
            int32_t px = in->nparams >= 1 ? in->params[1] : 0;
            int32_t py = in->nparams >= 2 ? in->params[2] : 0;
            mouse_common(t, cb, px, py, true, ovf);
        }
        return 0;

    case '~':
        csi_tilde(t, ovf);
        return 0;

    case 'u':   /* Kitty keyboard protocol (docs/04 §8.2) */
        csi_kitty(t, ovf);
        return 0;

    default:
        return 0;   /* unrecognized final byte: drop the sequence */
    }
}

/* --------------------------------------------------------------------------
 * Per-state handlers (return 0 consume, 1 reprocess the byte in GROUND)
 * ------------------------------------------------------------------------ */

static int parser_ground(tc_term* t, uint8_t c, tc_status* ovf) {
    tc_input* in = t->input;

    if (c == 0x1B) { in->state = TC_IN_ESC; return 0; }

    if (c < 0x20) {   /* C0 controls -> key events */
        switch (c) {
        case 0x08: case 0x7F:                      /* BS / DEL */
            acc(ovf, emit_key(t, 0, TC_KEY_BACKSPACE, 0, false));
            return 0;
        case 0x09:
            acc(ovf, emit_key(t, 0, TC_KEY_TAB, 0, false));
            return 0;
        case 0x0D:
            acc(ovf, emit_key(t, 0, TC_KEY_ENTER, 0, false));
            return 0;
        case 0x0A:                                 /* LF = Ctrl+J */
            acc(ovf, emit_key(t, 'j', TC_KEY_NONE, TC_MOD_CTRL, false));
            return 0;
        case 0x00:                                 /* NUL = Ctrl+Space */
            acc(ovf, emit_key(t, ' ', TC_KEY_NONE, TC_MOD_CTRL, false));
            return 0;
        default:
            if (c >= 0x01 && c <= 0x1A)            /* Ctrl+A .. Ctrl+Z */
                acc(ovf, emit_key(t, 0x60u + c, TC_KEY_NONE, TC_MOD_CTRL, false));
            else                                   /* Ctrl+\ ] ^ _ */
                acc(ovf, emit_key(t, 0x40u + c, TC_KEY_NONE, TC_MOD_CTRL, false));
            return 0;
        }
    }

    if (c == 0x7F) {
        acc(ovf, emit_key(t, 0, TC_KEY_BACKSPACE, 0, false));
        return 0;
    }

    if (c >= 0x80) {   /* UTF-8 (docs/04 §7) */
        if (t->text_mode == TC_TEXT_ASCII) {
            acc(ovf, emit_key(t, 0xFFFDu, TC_KEY_NONE, 0, false));
            return 0;
        }
        if (c >= 0xC2 && c <= 0xF4) {
            in->utf8[0]       = c;
            in->utf8_len      = 1;
            in->utf8_expected = (uint8_t)(c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4);
            in->state         = TC_IN_UTF8;
        } else {
            acc(ovf, emit_key(t, 0xFFFDu, TC_KEY_NONE, 0, false));   /* bad head */
        }
        return 0;
    }

    acc(ovf, emit_key(t, c, TC_KEY_NONE, 0, false));   /* printable ASCII */
    return 0;
}

static int parser_utf8(tc_term* t, uint8_t c, tc_status* ovf) {
    tc_input*  in = t->input;
    uint16_t   alt = in->alt_pending ? TC_MOD_ALT : 0;

    if ((c & 0xC0u) == 0x80u) {   /* continuation byte */
        if (in->utf8_len < in->utf8_expected) in->utf8[in->utf8_len++] = c;
        if (in->utf8_len == in->utf8_expected) {
            const char* p = (const char*)in->utf8;
            const char* end = p + in->utf8_len;
            uint32_t    cp  = 0xFFFDu;
            if (tc_text_utf8_next(&p, end, &cp) != TC_UTF8_OK) cp = 0xFFFDu;
            acc(ovf, emit_key(t, cp, TC_KEY_NONE, alt, false));
            in->utf8_len      = 0;
            in->utf8_expected = 0;
            in->alt_pending   = false;
            in->state         = TC_IN_GROUND;
        }
        return 0;
    }

    /* Non-continuation interrupted the sequence: replacement + reprocess. */
    acc(ovf, emit_key(t, 0xFFFDu, TC_KEY_NONE, alt, false));
    in->utf8_len      = 0;
    in->utf8_expected = 0;
    in->alt_pending   = false;
    in->state         = TC_IN_GROUND;
    return 1;
}

static int parser_esc(tc_term* t, uint8_t c, tc_status* ovf) {
    tc_input* in = t->input;

    switch (c) {
    case '[':
        in->state       = TC_IN_CSI;
        in->seq_len     = 0;
        in->nparams     = 0;
        in->params[0]   = 0;
        in->have_prefix = false;
        in->prefix      = 0;
        in->csi_drop    = false;
        return 0;
    case 'O':
        in->state = TC_IN_SS3;
        return 0;
    case ']':
        in->state  = TC_IN_OSC;
        in->seq_len = 0;
        in->st_esc  = false;
        return 0;
    case 'P':
        in->state  = TC_IN_DCS;
        in->seq_len = 0;
        in->st_esc  = false;
        return 0;
    case 0x1B:   /* ESC ESC: settle the first one immediately (docs/04 §11) */
        acc(ovf, emit_key(t, 0, TC_KEY_ESCAPE, 0, false));
        return 0;   /* stay in ESC; the second ESC is now pending */

    default:
        if (c < 0x20 || c == 0x7F) {
            /* C0 cannot extend a sequence: settle the ESC, reprocess. */
            acc(ovf, emit_key(t, 0, TC_KEY_ESCAPE, 0, false));
            in->state = TC_IN_GROUND;
            return 1;
        }
        if (c >= 0x80) {   /* Alt + UTF-8 char */
            in->alt_pending = true;
            if (c >= 0xC2 && c <= 0xF4) {
                in->utf8[0]       = c;
                in->utf8_len      = 1;
                in->utf8_expected = (uint8_t)(c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4);
                in->state         = TC_IN_UTF8;
            } else {
                acc(ovf, emit_key(t, 0xFFFDu, TC_KEY_NONE, TC_MOD_ALT, false));
                in->alt_pending = false;
                in->state       = TC_IN_GROUND;
            }
            return 0;
        }
        /* Alt + printable character */
        acc(ovf, emit_key(t, c, TC_KEY_NONE, TC_MOD_ALT, false));
        in->state = TC_IN_GROUND;
        return 0;
    }
}

static int parser_csi(tc_term* t, uint8_t c, tc_status* ovf) {
    tc_input* in = t->input;

    if (in->csi_drop) {
        /* Discarding an oversized/malformed sequence: swallow everything up
         * to the final byte, so no intermediate byte leaks out as text. */
        if (c >= 0x40 && c <= 0x7E) {
            in->csi_drop = false;
            in->state    = TC_IN_GROUND;
        }
        return 0;
    }

    if (++in->seq_len > TC_IN_SEQ_MAX) {   /* robustness: oversize -> drop */
        in->csi_drop = true;
        return 0;
    }

    if (c >= '0' && c <= '9') {   /* accumulate the current parameter */
        int64_t v = (int64_t)in->params[in->nparams] * 10 + (int64_t)(c - '0');
        if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;
        in->params[in->nparams] = (int32_t)v;
        return 0;
    }

    if (c == ';' || c == ':') {   /* parameter separator (':' is Kitty) */
        if (in->nparams + 1 >= TC_IN_PARAMS_MAX) {   /* too many -> drop */
            in->csi_drop = true;
            return 0;
        }
        in->nparams++;
        in->params[in->nparams] = 0;
        in->sep[in->nparams]    = c;
        return 0;
    }

    if (c == '<' || c == '=' || c == '>' || c == '?') {
        if (in->have_prefix) {   /* a second prefix byte is malformed */
            in->csi_drop = true;
            return 0;
        }
        in->have_prefix = true;
        in->prefix      = c;
        return 0;
    }

    if (c >= 0x40 && c <= 0x7E) {   /* final byte */
        uint8_t prev = in->state;
        (void)csi_final(t, c, ovf);
        /* The final handler may have moved on (X10 payload, paste state);
         * otherwise the sequence is complete and we return to GROUND. */
        if (in->state == prev) in->state = TC_IN_GROUND;
        return 0;
    }

    if (c >= 0x20 && c <= 0x2F) return 0;   /* intermediate byte: ignore */

    /* Anything else (C0, DEL, high bytes) aborts the sequence; reprocess. */
    in->state = TC_IN_GROUND;
    return 1;
}

static int parser_ss3(tc_term* t, uint8_t c, tc_status* ovf) {
    tc_input* in = t->input;
    tc_key    key;

    switch (c) {
    case 'A': key = TC_KEY_UP;    break;
    case 'B': key = TC_KEY_DOWN;  break;
    case 'C': key = TC_KEY_RIGHT; break;
    case 'D': key = TC_KEY_LEFT;  break;
    case 'H': key = TC_KEY_HOME;  break;
    case 'F': key = TC_KEY_END;   break;
    case 'P': key = TC_KEY_F1;    break;
    case 'Q': key = TC_KEY_F2;    break;
    case 'R': key = TC_KEY_F3;    break;
    case 'S': key = TC_KEY_F4;    break;
    default:
        in->state = TC_IN_GROUND;
        return 0;   /* unknown SS3 final: drop */
    }
    acc(ovf, emit_key(t, 0, key, 0, false));
    in->state = TC_IN_GROUND;
    return 0;
}

/* OSC / DCS share the same collector: content is discarded (nothing needed
 * in v1), terminated by BEL or ESC \ (ST), capped by the sequence limit. */
static int parser_osc_like(tc_term* t, uint8_t c, tc_status* ovf) {
    tc_input* in = t->input;
    (void)ovf;

    if (in->st_esc) {
        in->st_esc = false;
        if (c == '\\') { in->state = TC_IN_GROUND; return 0; }   /* ST */
        /* otherwise c is ordinary content */
    } else if (c == 0x1B) {
        in->st_esc = true;
        return 0;
    } else if (c == 0x07) {   /* BEL also terminates */
        in->state = TC_IN_GROUND;
        return 0;
    }

    if (++in->seq_len > TC_IN_SEQ_MAX) in->state = TC_IN_GROUND;
    return 0;
}

static int parser_paste(tc_term* t, uint8_t c, tc_status* ovf) {
    tc_input* in = t->input;

    in->paste_total++;
    if (in->paste_len < TC_IN_PASTE_CAP) {
        in->paste_buf[in->paste_len++] = (char)c;
    }   /* over the cap: truncate, keep matching the end sequence */

    /* Slide the 6-byte end-sequence window ("ESC [ 201 ~"). */
    memmove(in->paste_tail, in->paste_tail + 1, 5);
    in->paste_tail[5] = c;

    if (memcmp(in->paste_tail, "\x1b[201~", 6) == 0) {
        size_t content = in->paste_total >= 6 ? in->paste_total - 6 : 0;
        if (content > TC_IN_PASTE_CAP) content = TC_IN_PASTE_CAP;
        acc(ovf, emit_paste(t, content));
        in->paste_len   = 0;
        in->paste_total = 0;
        memset(in->paste_tail, 0, sizeof(in->paste_tail));
        in->state = TC_IN_GROUND;
    }
    return 0;
}

static int parser_x10(tc_term* t, uint8_t c, tc_status* ovf) {
    tc_input* in = t->input;

    if (in->x10_len < 3) {
        in->x10_buf[in->x10_len++] = c;
        if (in->x10_len == 3) {
            int32_t cb = (int32_t)in->x10_buf[0] - 32;
            int32_t px = (int32_t)in->x10_buf[1] - 32;
            int32_t py = (int32_t)in->x10_buf[2] - 32;
            mouse_common(t, cb, px, py, false, ovf);
            in->state = TC_IN_GROUND;
        }
        return 0;
    }
    in->state = TC_IN_GROUND;
    return 0;   /* not reachable: state exits after 3 bytes */
}

/* --------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------ */

tc_status tc_parser_feed(tc_term* t, const uint8_t* bytes, size_t len) {
    tc_input* in = t->input;
    tc_status ovf = TC_OK;
    size_t    i = 0;

    while (i < len) {
        uint8_t c = bytes[i];
        int     redo = 0;

        switch (in->state) {
        case TC_IN_GROUND: redo = parser_ground(t, c, &ovf); break;
        case TC_IN_UTF8:   redo = parser_utf8(t, c, &ovf);   break;
        case TC_IN_ESC:    redo = parser_esc(t, c, &ovf);    break;
        case TC_IN_CSI:    redo = parser_csi(t, c, &ovf);    break;
        case TC_IN_SS3:    redo = parser_ss3(t, c, &ovf);    break;
        case TC_IN_OSC:    redo = parser_osc_like(t, c, &ovf); break;
        case TC_IN_DCS:    redo = parser_osc_like(t, c, &ovf); break;
        case TC_IN_X10:    redo = parser_x10(t, c, &ovf);    break;
        case TC_IN_PASTE:  redo = parser_paste(t, c, &ovf);  break;
        default:
            in->state = TC_IN_GROUND;
            redo = 0;
            break;
        }

        if (!redo) i++;
    }

    /* Record the residual for the settle deadline (docs/04 §11). PASTE never
     * becomes a residual: an unterminated paste keeps accumulating. */
    if (in->state != TC_IN_GROUND && in->state != TC_IN_PASTE) {
        in->residual_state    = in->state;
        in->residual_arrive_ms = tc_input_now_ms();
    } else {
        in->residual_state = TC_IN_GROUND;
    }

    return ovf;
}
