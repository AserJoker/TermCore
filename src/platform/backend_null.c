#include <platform/backend.h>

#include <string.h>

/* --------------------------------------------------------------------------
 * Null backend: headless / CI / unit tests (docs/01 §4, docs/02 §3).
 *
 * It owns no handle and never touches a terminal. `enter` / `leave` become
 * pure state transitions that emit no bytes, sizes come from tc_term_set_size,
 * and writes are discarded until sinks land with the render layer.
 * ------------------------------------------------------------------------ */
typedef struct null_backend {
    tc_backend base;
    int32_t    cols;
    int32_t    rows;
} null_backend;

static void null_dispose(tc_backend* b) {
    (void)b;
}

static tc_status null_set_raw(tc_backend* b, bool on) {
    (void)b;
    (void)on;
    return TC_OK;
}

static tc_status null_get_size(tc_backend* b, int32_t* cols, int32_t* rows) {
    null_backend* n = (null_backend*)b;
    if (cols) *cols = n->cols;
    if (rows) *rows = n->rows;
    return TC_OK;
}

static tc_status null_get_size_px(tc_backend* b, int32_t* w, int32_t* h) {
    (void)b;
    if (w) *w = 0;
    if (h) *h = 0;
    return TC_ERR_UNSUPPORTED;
}

static tc_status null_set_cursor_visible(tc_backend* b, bool visible) {
    (void)b;
    (void)visible;
    return TC_OK;
}

static tc_status null_set_cursor_pos(tc_backend* b, int32_t x, int32_t y) {
    (void)b;
    (void)x;
    (void)y;
    return TC_OK;
}

static tc_status null_set_title(tc_backend* b, const char* utf8) {
    (void)b;
    (void)utf8;
    return TC_OK;
}

static tc_status null_write(tc_backend* b, const void* buf, size_t len, size_t* nwritten) {
    (void)b;
    (void)buf;
    if (nwritten) *nwritten = len;
    return TC_OK;
}

static const tc_backend_vtable g_null_vtable = {
    null_dispose,
    null_set_raw,
    null_get_size,
    null_get_size_px,
    null_set_cursor_visible,
    null_set_cursor_pos,
    null_set_title,
    null_write
};

tc_status tc_backend_create_null(const tc_allocator* alloc, tc_backend** out) {
    null_backend* n;

    if (!out) return TC_ERR_INVALID_ARG;
    *out = NULL;
    if (!alloc) alloc = tc_allocator_default();

    n = (null_backend*)alloc->alloc(alloc->ctx, sizeof(*n), sizeof(void*));
    if (!n) return TC_ERR_NOMEM;

    memset(n, 0, sizeof(*n));
    n->base.vt        = &g_null_vtable;
    n->base.kind      = TC_BACKEND_NULL;
    n->base.alloc     = alloc;
    n->base.self_size = sizeof(*n);
    n->cols           = 80;   /* docs/02 §3: headless default */
    n->rows           = 24;

    *out = &n->base;
    return TC_OK;
}
