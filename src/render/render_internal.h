#ifndef TERMCORE_RENDER_RENDER_INTERNAL_H
#define TERMCORE_RENDER_RENDER_INTERNAL_H

/* Internals shared by the render layer sources (surface.c, present.c).
 * Never installed. */

#include <termcore/tc_surface.h>

#include <control/term_internal.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tc_surface {
    const tc_allocator* alloc;
    tc_term*            term;         /* context only: allocator / text mode / caps */
    int32_t             cols;
    int32_t             rows;
    tc_text_mode        text_mode;
    bool                ambiguous_wide;

    tc_cell*            cells;        /* cols * rows, row-major */
    uint8_t*            dirty;        /* rows bytes, 1 = row changed */
};

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_RENDER_RENDER_INTERNAL_H */
