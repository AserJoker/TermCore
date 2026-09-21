#ifndef TERMCORE_PLATFORM_BACKEND_H
#define TERMCORE_PLATFORM_BACKEND_H

/* Internal platform abstraction (docs/01 §4, docs/08 §1). Never installed.
 *
 * A backend does four things: open/close terminal handles, move bytes, flip
 * terminal modes and report the size. It never parses escape sequences (that
 * is the input layer) and never decides capabilities (that is the capability
 * layer). It must also be safe to create on a non-TTY — the null backend
 * exists exactly for that.
 *
 * The vtable is intentionally thin: everything that is "just a sequence"
 * (alternate screen, mouse, focus, paste, Kitty, sync update, cursor shape)
 * lives in the control layer and goes through write(). Only the things that
 * need a platform API are vtable entries.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termcore/tc_memory.h>
#include <termcore/tc_platform.h>
#include <termcore/tc_status.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tc_backend tc_backend;

typedef struct tc_backend_vtable {
    void      (*dispose)(tc_backend* b);
    tc_status (*set_raw)(tc_backend* b, bool on);
    tc_status (*get_size)(tc_backend* b, int32_t* cols, int32_t* rows);
    tc_status (*get_size_px)(tc_backend* b, int32_t* w, int32_t* h);
    tc_status (*set_cursor_visible)(tc_backend* b, bool visible);
    tc_status (*set_cursor_pos)(tc_backend* b, int32_t x, int32_t y);
    tc_status (*set_title)(tc_backend* b, const char* utf8);
    /* Partial writes are the backend's job: loop until everything is out. */
    tc_status (*write)(tc_backend* b, const void* buf, size_t len, size_t* nwritten);
} tc_backend_vtable;

struct tc_backend {
    const tc_backend_vtable* vt;
    tc_backend_kind          kind;
    const tc_allocator*      alloc;
    size_t                   self_size;   /* so destroy() can free correctly */
};

/* Dispatches on `kind`; TC_BACKEND_NULL always succeeds, which is what makes
 * headless and non-TTY environments safe. */
tc_status tc_backend_create(tc_backend_kind kind, const tc_allocator* alloc, tc_backend** out);
void      tc_backend_destroy(tc_backend* b);

tc_status tc_backend_set_raw(tc_backend* b, bool on);
tc_status tc_backend_get_size(tc_backend* b, int32_t* cols, int32_t* rows);
tc_status tc_backend_get_size_px(tc_backend* b, int32_t* w, int32_t* h);
tc_status tc_backend_set_cursor_visible(tc_backend* b, bool visible);
tc_status tc_backend_set_cursor_pos(tc_backend* b, int32_t x, int32_t y);
tc_status tc_backend_set_title(tc_backend* b, const char* utf8);
tc_status tc_backend_write(tc_backend* b, const void* buf, size_t len, size_t* nwritten);

/* One factory per backend. Each returns TC_ERR_UNSUPPORTED when compiled for
 * another platform, so the dispatcher above needs no #ifdef. */
tc_status tc_backend_create_null(const tc_allocator* alloc, tc_backend** out);
tc_status tc_backend_create_win32(const tc_allocator* alloc, tc_backend** out);
tc_status tc_backend_create_posix(const tc_allocator* alloc, tc_backend** out);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_PLATFORM_BACKEND_H */
