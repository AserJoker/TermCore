#ifndef TERMCORE_TC_SURFACE_H
#define TERMCORE_TC_SURFACE_H

/* Render layer: off-screen cell grid + frame commit (docs/06).
 *
 * A surface is a plain grid of cells owned by the caller. Draw into it with
 * the put_cell / fill_rect / clear / draw_text primitives, then hand the final
 * frame to tc_present(), which diffs it against the terminal's known state
 * (front buffer, held by the term) and writes only what changed.
 *
 * Model: cell grid, not a render target. There is no blit / layer compositing
 * here — callers that want stacked panels assemble the final grid themselves.
 *
 * Hot paths (every draw call and tc_present) never allocate: the cell buffer
 * is allocated once at create/resize time.
 */

#include <stdbool.h>
#include <stdint.h>

#include <termcore/tc_export.h>
#include <termcore/tc_status.h>
#include <termcore/tc_text.h>

#ifdef __cplusplus
extern "C" {
#endif

/* tc_present takes a term handle; declared here so the header stays
 * self-contained. */
typedef struct tc_term tc_term_t;

/* --------------------------------------------------------------------------
 * POD types (docs/06 §2)
 * ------------------------------------------------------------------------ */

typedef enum tc_color_kind {
    TC_COLOR_DEFAULT = 0,   /* terminal default color */
    TC_COLOR_INDEXED = 1,   /* 0–255 indexed */
    TC_COLOR_RGB     = 2    /* 24-bit truecolor */
} tc_color_kind;

typedef struct tc_color {
    uint8_t kind;           /* tc_color_kind */
    uint8_t r, g, b;        /* valid in RGB mode */
    uint8_t idx;            /* valid in INDEXED mode */
    uint8_t reserved[3];
} tc_color;

typedef enum tc_style {
    TC_STYLE_NONE           = 0,
    TC_STYLE_BOLD           = 1u << 0,
    TC_STYLE_DIM            = 1u << 1,
    TC_STYLE_ITALIC         = 1u << 2,
    TC_STYLE_UNDERLINE      = 1u << 3,
    TC_STYLE_BLINK          = 1u << 4,
    TC_STYLE_REVERSE        = 1u << 5,
    TC_STYLE_HIDDEN         = 1u << 6,
    TC_STYLE_STRIKE         = 1u << 7,
    TC_STYLE_UNDERCURL      = 1u << 8,
    TC_STYLE_DOUBLE_UNDERLINE = 1u << 9
} tc_style;

typedef enum tc_cell_attr {
    TC_ATTR_NONE      = 0,
    TC_ATTR_WIDE_HEAD = 1u << 0,   /* first cell of a double-width cluster */
    TC_ATTR_WIDE_CONT = 1u << 1    /* second cell (empty content) */
} tc_cell_attr;

#define TC_CELL_TEXT_CAP 15

typedef struct tc_cell {
    tc_color fg;
    tc_color bg;
    uint16_t style;      /* tc_style bits */
    uint16_t attr;       /* tc_cell_attr bits */
    uint8_t  len;        /* valid bytes in text */
    char     text[TC_CELL_TEXT_CAP + 1];   /* UTF-8, NUL-terminated */
    uint8_t  reserved[2];
} tc_cell;

typedef struct tc_rect { int32_t x, y, w, h; uint32_t reserved[2]; } tc_rect;

/* Attribute group for draw_text (no character content). */
typedef struct tc_style_attr {
    tc_color fg;
    tc_color bg;
    uint16_t style;      /* tc_style bits */
    uint16_t reserved[3];
} tc_style_attr;

/* --------------------------------------------------------------------------
 * Lifecycle (docs/06 §3)
 * ------------------------------------------------------------------------ */

typedef struct tc_surface tc_surface_t;

/* Allocates one cell buffer, filled with blank cells; the whole grid is
 * marked dirty. `t` supplies the allocator, default text mode and capability
 * context; the surface is not bound to the screen. */
TC_API tc_status tc_surface_create(tc_term_t* t, int32_t cols, int32_t rows,
                                   tc_surface_t** out);

/* Reallocates and reuses the buffer; content keeps the top-left corner
 * (strategy may later become configurable). The whole grid is marked dirty. */
TC_API tc_status tc_surface_resize(tc_surface_t* s, int32_t cols, int32_t rows);

TC_API void tc_surface_destroy(tc_surface_t* s);

TC_API tc_status tc_surface_get_size(const tc_surface_t* s, int32_t* cols, int32_t* rows);
TC_API tc_status tc_surface_set_text_mode(tc_surface_t* s, tc_text_mode mode);
TC_API tc_status tc_surface_set_ambiguous_wide(tc_surface_t* s, bool wide);

/* --------------------------------------------------------------------------
 * Drawing (docs/06 §4)
 * ------------------------------------------------------------------------ */

TC_API tc_status tc_surface_put_cell(tc_surface_t* s, int32_t x, int32_t y,
                                     const tc_cell* c);
TC_API tc_status tc_surface_fill_rect(tc_surface_t* s, const tc_rect* r,
                                      const tc_cell* c);
TC_API tc_status tc_surface_clear(tc_surface_t* s);
TC_API tc_status tc_surface_draw_text(tc_surface_t* s, int32_t x, int32_t y,
                                      const char* utf8, const tc_style_attr* attr,
                                      tc_text_mode mode, int32_t* out_consumed_columns);

/* --------------------------------------------------------------------------
 * Commit and observation (docs/06 §9)
 * ------------------------------------------------------------------------ */

/* Diffs `s` against the term's front buffer and writes only the changes to the
 * terminal. Synchronous and blocking; the only action that touches the screen.
 * term must be ACTIVE. */
TC_API tc_status tc_present(tc_term_t* t, tc_surface_t* s);

/* Bounding box of all dirty rows since the last present. */
TC_API tc_status tc_surface_dirty_rect(const tc_surface_t* s, tc_rect* out);

/* --------------------------------------------------------------------------
 * Convenience constructors (docs/06 §15)
 * ------------------------------------------------------------------------ */

TC_API tc_cell  tc_cell_blank(void);
TC_API tc_color tc_color_rgb(uint8_t r, uint8_t g, uint8_t b);
TC_API tc_color tc_color_indexed(uint8_t idx);
TC_API tc_color tc_color_default(void);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_TC_SURFACE_H */
