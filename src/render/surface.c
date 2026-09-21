#include <termcore/tc_surface.h>

#include <control/term_internal.h>
#include <platform/backend.h>
#include <render/render_internal.h>

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Render layer: off-screen cell grid + frame commit (docs/06).
 *
 * Model: a surface is one cell grid owned by the caller. Draw primitives mark
 * affected rows dirty; tc_present diffs dirty rows against the term's front
 * mirror, generates escape sequences into the term's outbuf and writes once.
 *
 * The diff pipeline here is the v1 shape: dirty-row bitmap + per-cell compare
 * + run merging. Row hashes and per-row spans (docs/06 §5.1 / §5.2) are the
 * documented next step, not implemented yet.
 * ------------------------------------------------------------------------ */

/* --------------------------------------------------------------------------
 * Cell helpers
 * ------------------------------------------------------------------------ */

tc_cell tc_cell_blank(void) {
    tc_cell c;
    memset(&c, 0, sizeof(c));
    c.fg.kind = TC_COLOR_DEFAULT;
    c.bg.kind = TC_COLOR_DEFAULT;
    c.text[0] = '\0';
    return c;
}

tc_color tc_color_default(void) {
    tc_color c;
    memset(&c, 0, sizeof(c));
    return c;
}

tc_color tc_color_rgb(uint8_t r, uint8_t g, uint8_t b) {
    tc_color c;
    memset(&c, 0, sizeof(c));
    c.kind = TC_COLOR_RGB;
    c.r = r;
    c.g = g;
    c.b = b;
    return c;
}

tc_color tc_color_indexed(uint8_t idx) {
    tc_color c;
    memset(&c, 0, sizeof(c));
    c.kind = TC_COLOR_INDEXED;
    c.idx = idx;
    return c;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

tc_status tc_surface_create(tc_term_t* t, int32_t cols, int32_t rows,
                            tc_surface_t** out) {
    tc_term*           m = (tc_term*)t;
    const tc_allocator* alloc;
    tc_surface_t*      s;
    tc_cell*           cells;
    uint8_t*           dirty;

    if (!out) return TC_ERR_INVALID_ARG;
    *out = NULL;
    if (!t) return TC_ERR_INVALID_ARG;
    if (cols <= 0 || rows <= 0 || cols > 4096 || rows > 4096)
        return TC_ERR_INVALID_ARG;
    if (m->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    alloc = m->alloc ? m->alloc : tc_allocator_default();

    s = (tc_surface_t*)alloc->alloc(alloc->ctx, sizeof(*s), sizeof(void*));
    if (!s) return TC_ERR_NOMEM;
    memset(s, 0, sizeof(*s));

    cells = (tc_cell*)alloc->alloc(alloc->ctx,
                                   (size_t)cols * (size_t)rows * sizeof(tc_cell),
                                   sizeof(void*));
    if (!cells) {
        alloc->free(alloc->ctx, s, sizeof(*s));
        return TC_ERR_NOMEM;
    }

    dirty = (uint8_t*)alloc->alloc(alloc->ctx, (size_t)rows, sizeof(void*));
    if (!dirty) {
        alloc->free(alloc->ctx, cells, (size_t)cols * (size_t)rows * sizeof(tc_cell));
        alloc->free(alloc->ctx, s, sizeof(*s));
        return TC_ERR_NOMEM;
    }

    s->alloc          = alloc;
    s->term           = m;
    s->cols           = cols;
    s->rows           = rows;
    s->text_mode      = m->text_mode;
    s->ambiguous_wide = m->ambiguous_wide;
    s->cells          = cells;
    s->dirty          = dirty;

    /* Initial content: blank cells, whole grid dirty. */
    for (int32_t y = 0; y < rows; y++) {
        for (int32_t x = 0; x < cols; x++) cells[y * cols + x] = tc_cell_blank();
        dirty[y] = 1;
    }

    *out = s;
    return TC_OK;
}

tc_status tc_surface_resize(tc_surface_t* s, int32_t cols, int32_t rows) {
    const tc_allocator* alloc;
    tc_cell*           cells;
    uint8_t*           dirty;
    int32_t            copy_w;
    int32_t            copy_h;

    if (!s) return TC_ERR_INVALID_ARG;
    if (cols <= 0 || rows <= 0 || cols > 4096 || rows > 4096)
        return TC_ERR_INVALID_ARG;

    alloc = s->alloc;

    cells = (tc_cell*)alloc->alloc(alloc->ctx,
                                   (size_t)cols * (size_t)rows * sizeof(tc_cell),
                                   sizeof(void*));
    if (!cells) return TC_ERR_NOMEM;

    dirty = (uint8_t*)alloc->alloc(alloc->ctx, (size_t)rows, sizeof(void*));
    if (!dirty) {
        alloc->free(alloc->ctx, cells, (size_t)cols * (size_t)rows * sizeof(tc_cell));
        return TC_ERR_NOMEM;
    }

    /* Keep the top-left corner, blank the rest. */
    copy_w = cols < s->cols ? cols : s->cols;
    copy_h = rows < s->rows ? rows : s->rows;

    for (int32_t y = 0; y < rows; y++) {
        tc_cell* row = cells + (size_t)y * cols;
        for (int32_t x = 0; x < cols; x++) row[x] = tc_cell_blank();
        if (y < copy_h)
            memcpy(row, s->cells + (size_t)y * s->cols,
                   (size_t)copy_w * sizeof(tc_cell));
        dirty[y] = 1;
    }

    alloc->free(alloc->ctx, s->dirty, (size_t)s->rows);
    alloc->free(alloc->ctx, s->cells, (size_t)s->cols * (size_t)s->rows * sizeof(tc_cell));

    s->cols  = cols;
    s->rows  = rows;
    s->cells = cells;
    s->dirty = dirty;
    return TC_OK;
}

void tc_surface_destroy(tc_surface_t* s) {
    if (!s) return;
    s->alloc->free(s->alloc->ctx, s->dirty, (size_t)s->rows);
    s->alloc->free(s->alloc->ctx, s->cells,
                   (size_t)s->cols * (size_t)s->rows * sizeof(tc_cell));
    s->alloc->free(s->alloc->ctx, s, sizeof(*s));
}

tc_status tc_surface_get_size(const tc_surface_t* s, int32_t* cols, int32_t* rows) {
    if (!s || (!cols && !rows)) return TC_ERR_INVALID_ARG;
    if (cols) *cols = s->cols;
    if (rows) *rows = s->rows;
    return TC_OK;
}

tc_status tc_surface_set_text_mode(tc_surface_t* s, tc_text_mode mode) {
    if (!s) return TC_ERR_INVALID_ARG;
    if (mode != TC_TEXT_ASCII && mode != TC_TEXT_UNICODE) return TC_ERR_INVALID_ARG;
    s->text_mode = mode;
    return TC_OK;
}

tc_status tc_surface_set_ambiguous_wide(tc_surface_t* s, bool wide) {
    if (!s) return TC_ERR_INVALID_ARG;
    s->ambiguous_wide = wide;
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * Drawing (docs/06 §4, §4.1)
 * ------------------------------------------------------------------------ */

static tc_cell* cell_at(tc_surface_t* s, int32_t x, int32_t y) {
    return &s->cells[(size_t)y * s->cols + x];
}

static void mark_dirty(tc_surface_t* s, int32_t y) {
    if (y >= 0 && y < s->rows) s->dirty[y] = 1;
}

/* Clear a cell's wide-character partner when one half is overwritten
 * (docs/06 §4.1). */
static void clear_wide_partner(tc_surface_t* s, int32_t x, int32_t y) {
    tc_cell* c = cell_at(s, x, y);

    if (c->attr & TC_ATTR_WIDE_HEAD) {
        if (x + 1 < s->cols) {
            tc_cell* n = cell_at(s, x + 1, y);
            if (n->attr & TC_ATTR_WIDE_CONT) *n = tc_cell_blank();
        }
    } else if (c->attr & TC_ATTR_WIDE_CONT) {
        if (x > 0) {
            tc_cell* p = cell_at(s, x - 1, y);
            if (p->attr & TC_ATTR_WIDE_HEAD) *p = tc_cell_blank();
        }
    }
}

tc_status tc_surface_put_cell(tc_surface_t* s, int32_t x, int32_t y,
                              const tc_cell* c) {
    tc_cell* dst;
    tc_cell  old;

    if (!s || !c) return TC_ERR_INVALID_ARG;
    if (x < 0 || y < 0 || x >= s->cols || y >= s->rows) return TC_ERR_INVALID_ARG;

    dst = cell_at(s, x, y);
    old = *dst;

    /* The old cell is being replaced: clear its wide-character partner first,
     * unless the new value keeps the pairing (docs/06 §4.1). */
    if ((old.attr & TC_ATTR_WIDE_HEAD) &&
        !(c->attr & TC_ATTR_WIDE_HEAD) && x + 1 < s->cols &&
        (cell_at(s, x + 1, y)->attr & TC_ATTR_WIDE_CONT))
        *cell_at(s, x + 1, y) = tc_cell_blank();
    else if ((old.attr & TC_ATTR_WIDE_CONT) &&
             !(c->attr & TC_ATTR_WIDE_CONT) && x > 0 &&
             (cell_at(s, x - 1, y)->attr & TC_ATTR_WIDE_HEAD))
        *cell_at(s, x - 1, y) = tc_cell_blank();

    *dst = *c;

    if (c->attr & TC_ATTR_WIDE_HEAD) {
        /* A fresh HEAD overwrites its partner slot unconditionally. */
        if (x + 1 < s->cols) {
            tc_cell* n = cell_at(s, x + 1, y);
            *n = tc_cell_blank();
            n->attr = TC_ATTR_WIDE_CONT;
        }
    } else if (c->attr & TC_ATTR_WIDE_CONT) {
        /* Orphan CONT (no HEAD on its left) degrades to a blank (docs/06 §4.1). */
        if (x == 0 || !(cell_at(s, x - 1, y)->attr & TC_ATTR_WIDE_HEAD))
            *dst = tc_cell_blank();
    } else if (x + 1 < s->cols &&
               (cell_at(s, x + 1, y)->attr & TC_ATTR_WIDE_CONT)) {
        /* A plain cell replaced the HEAD that owned the CONT on the right:
         * that CONT is now orphaned. */
        *cell_at(s, x + 1, y) = tc_cell_blank();
    }

    mark_dirty(s, y);
    return TC_OK;
}

tc_status tc_surface_fill_rect(tc_surface_t* s, const tc_rect* r, const tc_cell* c) {
    int32_t x0, y0, x1, y1;

    if (!s || !r || !c) return TC_ERR_INVALID_ARG;
    if (r->w < 0 || r->h < 0) return TC_ERR_INVALID_ARG;

    x0 = r->x;
    y0 = r->y;
    x1 = r->x + r->w;
    y1 = r->y + r->h;

    /* Out of bounds entirely -> error; partially -> clip. */
    if (x1 <= 0 || y1 <= 0 || x0 >= s->cols || y0 >= s->rows)
        return TC_ERR_INVALID_ARG;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->cols) x1 = s->cols;
    if (y1 > s->rows) y1 = s->rows;

    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = x0; x < x1; x++) {
            /* Wide heads inside the rect need their CONT partner written too. */
            tc_cell* dst = cell_at(s, x, y);
            clear_wide_partner(s, x, y);
            *dst = *c;
            if ((c->attr & TC_ATTR_WIDE_HEAD) && x + 1 < s->cols &&
                x + 1 >= x0 && x + 1 < x1) {
                tc_cell* n = cell_at(s, x + 1, y);
                *n = tc_cell_blank();
                n->attr = TC_ATTR_WIDE_CONT;
            }
        }
        mark_dirty(s, y);
    }
    return TC_OK;
}

tc_status tc_surface_clear(tc_surface_t* s) {
    if (!s) return TC_ERR_INVALID_ARG;

    for (int32_t y = 0; y < s->rows; y++) {
        tc_cell* row = s->cells + (size_t)y * s->cols;
        for (int32_t x = 0; x < s->cols; x++) row[x] = tc_cell_blank();
        s->dirty[y] = 1;
    }
    return TC_OK;
}

/* Writes a zero-width cluster onto the previous cell (docs/06 §4.1). */
static void append_to_previous(tc_surface_t* s, int32_t x, int32_t y,
                               const char* bytes, size_t len) {
    tc_cell* prev = NULL;

    if (x > 0) prev = cell_at(s, x - 1, y);
    if (!prev) return;

    size_t room = TC_CELL_TEXT_CAP - prev->len;
    if (room == 0) return;   /* cluster dropped */

    if (len > room) len = room;
    memcpy(prev->text + prev->len, bytes, len);
    prev->len = (uint8_t)(prev->len + len);
    prev->text[prev->len] = '\0';
    mark_dirty(s, y);
}

/* Appends one grapheme cluster to a cell, applying the width rules. Returns
 * the number of columns consumed (0 / 1 / 2), or -1 when the row boundary
 * stopped the write. */
static int32_t write_cluster(tc_surface_t* s, int32_t x, int32_t y,
                             const tc_grapheme* g) {
    tc_cell c;
    int32_t w;
    size_t  blen = (size_t)(g->end - g->begin);

    w = tc_text_grapheme_width(g, s->text_mode, s->ambiguous_wide);

    if (w == 0) {
        append_to_previous(s, x, y, g->begin, blen);
        return 0;
    }
    if (x + w > s->cols) return -1;   /* stop, do not wrap */

    memset(&c, 0, sizeof(c));
    c.fg.kind = TC_COLOR_DEFAULT;
    c.bg.kind = TC_COLOR_DEFAULT;
    c.len     = (uint8_t)(blen > TC_CELL_TEXT_CAP ? TC_CELL_TEXT_CAP : blen);
    memcpy(c.text, g->begin, c.len);
    c.text[c.len] = '\0';

    if (w == 2) {
        c.attr = TC_ATTR_WIDE_HEAD;
        if (tc_surface_put_cell(s, x, y, &c) != TC_OK) return -1;
        tc_cell cont = tc_cell_blank();
        cont.attr    = TC_ATTR_WIDE_CONT;
        (void)tc_surface_put_cell(s, x + 1, y, &cont);
    } else {
        (void)tc_surface_put_cell(s, x, y, &c);
    }
    return w;
}

tc_status tc_surface_draw_text(tc_surface_t* s, int32_t x, int32_t y,
                               const char* utf8, const tc_style_attr* attr,
                               tc_text_mode mode, int32_t* out_consumed_columns) {
    size_t      off   = 0;
    size_t      len;
    int32_t     cx    = x;
    int32_t     consumed = 0;
    tc_grapheme g;

    if (!s || !utf8) return TC_ERR_INVALID_ARG;
    if (x < 0 || y < 0 || x >= s->cols || y >= s->rows) return TC_ERR_INVALID_ARG;

    if (mode != TC_TEXT_ASCII && mode != TC_TEXT_UNICODE) return TC_ERR_INVALID_ARG;
    if (mode == TC_TEXT_ASCII) {
        /* One byte per cell, no cluster merging (docs/06 §4.1). */
        len = strlen(utf8);
        while (off < len && cx < s->cols) {
            unsigned char b = (unsigned char)utf8[off];
            if (b < 0x20 || b == 0x7f) { off++; continue; }   /* control: skip */
            tc_cell c = tc_cell_blank();
            if (attr) { c.fg = attr->fg; c.bg = attr->bg; c.style = attr->style; }
            c.len  = 1;
            c.text[0] = (char)b;
            (void)tc_surface_put_cell(s, cx, y, &c);
            cx++;
            consumed++;
            off++;
        }
        if (out_consumed_columns) *out_consumed_columns = consumed;
        return TC_OK;
    }

    len = strlen(utf8);
    while (off < len) {
        if (!tc_text_grapheme_next_off(utf8, len, &off, &g)) break;

        int32_t w = write_cluster(s, cx, y, &g);
        if (w < 0) break;   /* row boundary */

        if (w > 0) {
            if (attr) {
                tc_cell* c = cell_at(s, cx, y);
                c->fg      = attr->fg;
                c->bg      = attr->bg;
                c->style   = attr->style;
            }
            cx += w;
            consumed += w;
        }
    }

    if (out_consumed_columns) *out_consumed_columns = consumed;
    return TC_OK;
}

tc_status tc_surface_dirty_rect(const tc_surface_t* s, tc_rect* out) {
    int32_t y0, y1;

    if (!s || !out) return TC_ERR_INVALID_ARG;

    /* Dirty-row granularity: the bounding box spans every dirty row at full
     * width (column tracking is the per-row span optimization, docs/06 §5.2,
     * not implemented in the v1 shape). */
    y0 = s->rows;
    y1 = -1;
    for (int32_t y = 0; y < s->rows; y++) {
        if (!s->dirty[y]) continue;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }

    if (y1 < y0) {
        out->x = 0; out->y = 0; out->w = 0; out->h = 0;
        return TC_OK;
    }

    out->x = 0;
    out->y = y0;
    out->w = s->cols;
    out->h = y1 - y0 + 1;
    return TC_OK;
}
