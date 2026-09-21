#include <termcore/tc_surface.h>

#include <caps/caps_internal.h>
#include <control/term_internal.h>
#include <platform/backend.h>
#include <render/render_internal.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Frame commit (docs/06 §6, §7, §9).
 *
 * tc_present diffs the surface against the term's front mirror (the terminal's
 * known state from the last successful present) and writes only the changed
 * runs, minimized for cursor moves and SGR resets. Output lands in the term's
 * reusable outbuf and is written once.
 *
 * The v1 diff pipeline: dirty-row bitmap -> per-cell compare -> run merging.
 * Row hashes and per-row spans (docs/06 §5.1 / §5.2) are the documented next
 * step, not implemented yet.
 * ------------------------------------------------------------------------ */

/* Cursor addressing is 1-based (docs/06 §6.2). */
#define TC_SEQ_CUP "\x1b[%d;%dH"

/* --------------------------------------------------------------------------
 * outbuf management (owned by term)
 * ------------------------------------------------------------------------ */

static tc_status outbuf_ensure(tc_term* t, size_t extra) {
    size_t need;
    size_t cap;
    char*  p;

    if (!t->outbuf) {
        cap = 4096;
        if (extra > cap) cap = extra;
        p = (char*)t->alloc->alloc(t->alloc->ctx, cap, sizeof(void*));
        if (!p) return TC_ERR_NOMEM;
        t->outbuf    = p;
        t->outbuf_cap = cap;
        t->outbuf_len = 0;
        return TC_OK;
    }

    need = t->outbuf_len + extra + 1;
    if (need <= t->outbuf_cap) return TC_OK;

    cap = t->outbuf_cap;
    while (cap < need) cap *= 2;

    p = (char*)t->alloc->realloc(t->alloc->ctx, t->outbuf, t->outbuf_cap, cap,
                                 sizeof(void*));
    if (!p) return TC_ERR_NOMEM;
    t->outbuf     = p;
    t->outbuf_cap = cap;
    return TC_OK;
}

static tc_status outbuf_append(tc_term* t, const char* bytes, size_t len) {
    tc_status st = outbuf_ensure(t, len);
    if (st != TC_OK) return st;
    memcpy(t->outbuf + t->outbuf_len, bytes, len);
    t->outbuf_len += len;
    t->outbuf[t->outbuf_len] = '\0';
    return TC_OK;
}

static tc_status outbuf_printf(tc_term* t, const char* fmt, ...) {
    /* The two formats used here (CUP and SGR) have bounded output; a small
     * stack scratch is enough, but grow via outbuf_ensure if ever needed. */
    char     tmp[64];
    va_list  ap;
    int      n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return TC_ERR_FAIL;
    if ((size_t)n >= sizeof(tmp)) {
        tc_status st = outbuf_ensure(t, (size_t)n + 1);
        if (st != TC_OK) return st;
        va_start(ap, fmt);
        n = vsnprintf(t->outbuf + t->outbuf_len, t->outbuf_cap - t->outbuf_len,
                      fmt, ap);
        va_end(ap);
        if (n < 0) return TC_ERR_FAIL;
        t->outbuf_len += (size_t)n;
        t->outbuf[t->outbuf_len] = '\0';
        return TC_OK;
    }
    return outbuf_append(t, tmp, (size_t)n);
}

/* --------------------------------------------------------------------------
 * front buffer (owned by term)
 * ------------------------------------------------------------------------ */

/* Allocates or reallocates the front mirror to match the term size. Returns
 * TC_OK when front_cols/front_rows equal the term size afterwards. */
static tc_status front_ensure(tc_term* t) {
    int32_t  cols = t->cols;
    int32_t  rows = t->rows;
    tc_cell* f;

    if (t->front && t->front_cols == cols && t->front_rows == rows)
        return TC_OK;

    if (t->front) {
        t->alloc->free(t->alloc->ctx, t->front,
                       (size_t)t->front_cols * (size_t)t->front_rows * sizeof(tc_cell));
        t->front = NULL;
    }

    f = (tc_cell*)t->alloc->alloc(t->alloc->ctx,
                                  (size_t)cols * (size_t)rows * sizeof(tc_cell),
                                  sizeof(void*));
    if (!f) return TC_ERR_NOMEM;

    /* Blank: the terminal is "unknown" until the first present (docs/06 §5). */
    for (int32_t y = 0; y < rows; y++)
        for (int32_t x = 0; x < cols; x++)
            f[(size_t)y * cols + x] = tc_cell_blank();

    t->front      = f;
    t->front_cols = cols;
    t->front_rows = rows;
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * SGR (docs/06 §6.1, §8)
 * ------------------------------------------------------------------------ */

static bool color_equal(const tc_color* a, const tc_color* b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TC_COLOR_RGB:     return a->r == b->r && a->g == b->g && a->b == b->b;
    case TC_COLOR_INDEXED: return a->idx == b->idx;
    default:               return true;   /* both default */
    }
}

/* Colour degradation at output time (docs/03 ?3): the diff compares the
 * caller's raw colours, but what reaches the terminal is the degradation of
 * the session's effective caps. `caps` may be NULL (term without caps state),
 * in which case the colour passes through unchanged. */
static tc_color downgrade_color(const tc_caps* caps, const tc_color* c) {
    tc_color out = *c;
    if (!caps) return out;
    if (c->kind == TC_COLOR_RGB) {
        if (tc_caps_downgrade_rgb(caps, c->r, c->g, c->b, &out) != TC_OK)
            out = *c;
    } else if (c->kind == TC_COLOR_INDEXED) {
        if (tc_caps_downgrade_indexed(caps, c->idx, &out) != TC_OK)
            out = *c;
    }
    return out;
}

/* Appends the SGR parameters for one cell as a *transition* from `prev` (the
 * last SGR we emitted, or NULL on the frame's first run: the terminal state
 * is unknown there, so a base reset is included). The caller has already
 * verified that the cell differs from `prev`, so only the changed attributes
 * are re-stated (docs/06 §6.1). Returns the number of parameter characters
 * written, or -1 on allocation failure. */
static int sgr_write(tc_term* t, const tc_cell* c, const tc_cell* prev) {
    char     params[96];
    int      n = 0;
    int      i;
    bool     base_reset = !prev || prev->style != c->style;
    bool     fg_same    = prev && color_equal(&prev->fg, &c->fg);
    bool     bg_same    = prev && color_equal(&prev->bg, &c->bg);

    /* A style-set change forces a base reset: "0" wipes every attribute
     * (colors included), so any non-default color must be re-stated below. */
    if (base_reset) n += sprintf(params + n, "0;");

    /* Style bits. */
    for (i = 0; i < 10; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        if (!(c->style & bit)) continue;
        switch (bit) {
        case TC_STYLE_BOLD:            n += sprintf(params + n, "1;"); break;
        case TC_STYLE_DIM:             n += sprintf(params + n, "2;"); break;
        case TC_STYLE_ITALIC:          n += sprintf(params + n, "3;"); break;
        case TC_STYLE_UNDERLINE:       n += sprintf(params + n, "4;"); break;
        case TC_STYLE_BLINK:           n += sprintf(params + n, "5;"); break;
        case TC_STYLE_REVERSE:         n += sprintf(params + n, "7;"); break;
        case TC_STYLE_HIDDEN:          n += sprintf(params + n, "8;"); break;
        case TC_STYLE_STRIKE:          n += sprintf(params + n, "9;"); break;
        case TC_STYLE_UNDERCURL:       n += sprintf(params + n, "4:3;"); break;
        case TC_STYLE_DOUBLE_UNDERLINE:n += sprintf(params + n, "4:2;"); break;
        default: break;
        }
    }

    /* Foreground: only when it actually changes, or was wiped by the reset
     * (a default color needs nothing after "0" ��� the reset already set it). */
    switch (c->fg.kind) {
    case TC_COLOR_DEFAULT: if (!fg_same && !base_reset) n += sprintf(params + n, "39;"); break;
    case TC_COLOR_INDEXED: if (!fg_same || base_reset) n += sprintf(params + n, "38;5;%u;", c->fg.idx); break;
    case TC_COLOR_RGB:     if (!fg_same || base_reset) n += sprintf(params + n, "38;2;%u;%u;%u;",
                                                                    c->fg.r, c->fg.g, c->fg.b); break;
    default: break;
    }

    /* Background. */
    switch (c->bg.kind) {
    case TC_COLOR_DEFAULT: if (!bg_same && !base_reset) n += sprintf(params + n, "49;"); break;
    case TC_COLOR_INDEXED: if (!bg_same || base_reset) n += sprintf(params + n, "48;5;%u;", c->bg.idx); break;
    case TC_COLOR_RGB:     if (!bg_same || base_reset) n += sprintf(params + n, "48;2;%u;%u;%u;",
                                                                    c->bg.r, c->bg.g, c->bg.b); break;
    default: break;
    }

    if (n == 0) {
        /* Safety net: the caller skips identical runs, so reaching here means
         * the state actually changed but produced no parameters. */
        if (outbuf_append(t, "0", 1) != TC_OK) return -1;
        return 1;
    }

    params[n - 1] = '\0';   /* drop the trailing ';' */
    if (outbuf_append(t, params, (size_t)n - 1) != TC_OK) return -1;
    return n - 1;
}

/* --------------------------------------------------------------------------
 * tc_present
 * ------------------------------------------------------------------------ */

tc_status tc_present(tc_term_t* t, tc_surface_t* s) {
    tc_term*    m = (tc_term*)t;
    tc_status   st;
    int32_t     cmp_cols;
    int32_t     cmp_rows;
    int32_t     vcur_x = -1;   /* virtual cursor, 0-based; (-1,-1) forces the */
    int32_t     vcur_y = -1;   /* first run to emit an explicit CUP */
    bool        have_sgr = false;
    tc_cell     cur_sgr_cell;   /* last SGR we emitted for */
    const tc_caps* caps = m->caps ? &m->caps->caps : NULL;

    if (!t || !s) return TC_ERR_INVALID_ARG;
    if (m->state != TC_TERM_STATE_ACTIVE) return TC_ERR_STATE;
    if (!m->backend) return TC_ERR_STATE;

    st = front_ensure(m);
    if (st != TC_OK) return st;

    /* Size reconciliation: diff the smaller of surface / terminal (docs/06 §3). */
    cmp_cols = s->cols < m->cols ? s->cols : m->cols;
    cmp_rows = s->rows < m->rows ? s->rows : m->rows;

    /* Start fresh for this frame. */
    m->outbuf_len = 0;

    for (int32_t y = 0; y < cmp_rows; y++) {
        if (!s->dirty[y]) continue;

        int32_t x = 0;
        while (x < cmp_cols) {
            const tc_cell* back  = &s->cells[(size_t)y * s->cols + x];
            const tc_cell* front = &m->front[(size_t)y * m->front_cols + x];

            if (back->len == front->len &&
                memcmp(back->text, front->text, back->len) == 0 &&
                back->style == front->style &&
                back->attr  == front->attr &&
                back->fg.kind == front->fg.kind &&
                back->bg.kind == front->bg.kind &&
                (back->fg.kind != TC_COLOR_RGB ||
                 (back->fg.r == front->fg.r && back->fg.g == front->fg.g &&
                  back->fg.b == front->fg.b)) &&
                (back->fg.kind != TC_COLOR_INDEXED ||
                 back->fg.idx == front->fg.idx) &&
                (back->bg.kind != TC_COLOR_RGB ||
                 (back->bg.r == front->bg.r && back->bg.g == front->bg.g &&
                  back->bg.b == front->bg.b)) &&
                (back->bg.kind != TC_COLOR_INDEXED ||
                 back->bg.idx == front->bg.idx)) {
                x++;
                continue;
            }

            /* Start of a run: extend while cells change and share attributes. */
            int32_t run_start = x;
            int32_t run_end   = x + 1;
            while (run_end < cmp_cols) {
                const tc_cell* nb = &s->cells[(size_t)y * s->cols + run_end];
                const tc_cell* nf = &m->front[(size_t)y * m->front_cols + run_end];

                bool same_as_front =
                    nb->len == nf->len &&
                    memcmp(nb->text, nf->text, nb->len) == 0 &&
                    nb->style == nf->style && nb->attr == nf->attr &&
                    nb->fg.kind == nf->fg.kind && nb->bg.kind == nf->bg.kind &&
                    (nb->fg.kind != TC_COLOR_RGB ||
                     (nb->fg.r == nf->fg.r && nb->fg.g == nf->fg.g &&
                      nb->fg.b == nf->fg.b)) &&
                    (nb->fg.kind != TC_COLOR_INDEXED ||
                     nb->fg.idx == nf->fg.idx) &&
                    (nb->bg.kind != TC_COLOR_RGB ||
                     (nb->bg.r == nf->bg.r && nb->bg.g == nf->bg.g &&
                      nb->bg.b == nf->bg.b)) &&
                    (nb->bg.kind != TC_COLOR_INDEXED ||
                     nb->bg.idx == nf->bg.idx);

                if (same_as_front) break;   /* unchanged cell ends the run */

                const tc_cell* prev = &s->cells[(size_t)y * s->cols + run_end - 1];
                if (nb->fg.kind != prev->fg.kind || nb->bg.kind != prev->bg.kind ||
                    nb->style != prev->style ||
                    nb->fg.kind == TC_COLOR_RGB &&
                        (nb->fg.r != prev->fg.r || nb->fg.g != prev->fg.g ||
                         nb->fg.b != prev->fg.b) ||
                    nb->fg.kind == TC_COLOR_INDEXED && nb->fg.idx != prev->fg.idx ||
                    nb->bg.kind == TC_COLOR_RGB &&
                        (nb->bg.r != prev->bg.r || nb->bg.g != prev->bg.g ||
                         nb->bg.b != prev->bg.b) ||
                    nb->bg.kind == TC_COLOR_INDEXED && nb->bg.idx != prev->bg.idx) {
                    break;   /* different attributes start a new run */
                }

                run_end++;
            }

            /* Cursor placement (1-based). */
            if (vcur_x != run_start || vcur_y != y) {
                if (outbuf_printf(m, TC_SEQ_CUP, y + 1, run_start + 1) != TC_OK)
                    return TC_ERR_NOMEM;
                vcur_x = run_start;
                vcur_y = y;
            }

            /* SGR: emit only when it differs from the current state. The
             * comparison and the emission both work on the degraded copy:
             * two different raw colours that degrade to the same terminal
             * colour do not re-state SGR. */
            const tc_cell* head = &s->cells[(size_t)y * s->cols + run_start];
            const tc_cell* prev = have_sgr ? &cur_sgr_cell : NULL;
            tc_cell        head_d = *head;
            head_d.fg = downgrade_color(caps, &head->fg);
            head_d.bg = downgrade_color(caps, &head->bg);
            if (!have_sgr ||
                head_d.style != cur_sgr_cell.style ||
                !color_equal(&head_d.fg, &cur_sgr_cell.fg) ||
                !color_equal(&head_d.bg, &cur_sgr_cell.bg)) {
                /* sgr_write emits the parameters without the CSI envelope;
                 * assemble "\x1b[<params>m" ourselves. */
                if (outbuf_append(m, "\x1b[", 2) != TC_OK) return TC_ERR_NOMEM;
                if (sgr_write(m, &head_d, prev) < 0) return TC_ERR_NOMEM;
                if (outbuf_append(m, "m", 1) != TC_OK) return TC_ERR_NOMEM;
                cur_sgr_cell = head_d;
                have_sgr = true;
            }

            /* Emit the run's text (skip WIDE_CONT partner cells). */
            for (int32_t i = run_start; i < run_end; i++) {
                const tc_cell* c = &s->cells[(size_t)y * s->cols + i];
                if (c->attr & TC_ATTR_WIDE_CONT) continue;
                if (c->len == 0) continue;
                if (outbuf_append(m, c->text, c->len) != TC_OK)
                    return TC_ERR_NOMEM;
            }

            /* Advance the virtual cursor past the whole run. */
            vcur_x += run_end - run_start;

            x = run_end;
        }
    }

    /* Flush once (docs/06 §7). */
    if (m->outbuf_len > 0) {
        size_t nwritten = 0;
        st = tc_backend_write(m->backend, m->outbuf, m->outbuf_len, &nwritten);
        if (st != TC_OK) return st;   /* front NOT updated; retried next frame */
    }

    /* Success: sync the dirty rows into front and clear the surface dirty bits. */
    for (int32_t y = 0; y < cmp_rows; y++) {
        if (!s->dirty[y]) continue;
        memcpy(m->front + (size_t)y * m->front_cols,
               s->cells + (size_t)y * s->cols,
               (size_t)cmp_cols * sizeof(tc_cell));
        s->dirty[y] = 0;
    }

    return TC_OK;
}
