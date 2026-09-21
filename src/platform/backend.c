#include <platform/backend.h>

tc_status tc_backend_create(tc_backend_kind kind, const tc_allocator* alloc, tc_backend** out) {
    if (!out) return TC_ERR_INVALID_ARG;
    *out = NULL;
    if (!alloc) alloc = tc_allocator_default();

    switch (kind) {
    case TC_BACKEND_NULL:   return tc_backend_create_null(alloc, out);
    case TC_BACKEND_WIN32:  return tc_backend_create_win32(alloc, out);
    case TC_BACKEND_POSIX:  return tc_backend_create_posix(alloc, out);
    default:                return TC_ERR_UNSUPPORTED;
    }
}

void tc_backend_destroy(tc_backend* b) {
    const tc_allocator* alloc;
    if (!b) return;

    if (b->vt && b->vt->dispose) b->vt->dispose(b);

    alloc = b->alloc ? b->alloc : tc_allocator_default();
    alloc->free(alloc->ctx, b, b->self_size);
}

tc_status tc_backend_set_raw(tc_backend* b, bool on) {
    if (!b || !b->vt || !b->vt->set_raw) return TC_ERR_UNSUPPORTED;
    return b->vt->set_raw(b, on);
}

tc_status tc_backend_get_size(tc_backend* b, int32_t* cols, int32_t* rows) {
    if (!b || !b->vt || !b->vt->get_size) return TC_ERR_UNSUPPORTED;
    return b->vt->get_size(b, cols, rows);
}

tc_status tc_backend_get_size_px(tc_backend* b, int32_t* w, int32_t* h) {
    if (!b || !b->vt || !b->vt->get_size_px) return TC_ERR_UNSUPPORTED;
    return b->vt->get_size_px(b, w, h);
}

tc_status tc_backend_set_cursor_visible(tc_backend* b, bool visible) {
    if (!b || !b->vt || !b->vt->set_cursor_visible) return TC_ERR_UNSUPPORTED;
    return b->vt->set_cursor_visible(b, visible);
}

tc_status tc_backend_set_cursor_pos(tc_backend* b, int32_t x, int32_t y) {
    if (!b || !b->vt || !b->vt->set_cursor_pos) return TC_ERR_UNSUPPORTED;
    return b->vt->set_cursor_pos(b, x, y);
}

tc_status tc_backend_set_title(tc_backend* b, const char* utf8) {
    if (!b || !b->vt || !b->vt->set_title) return TC_ERR_UNSUPPORTED;
    return b->vt->set_title(b, utf8);
}

tc_status tc_backend_write(tc_backend* b, const void* buf, size_t len, size_t* nwritten) {
    if (!b || !b->vt || !b->vt->write) return TC_ERR_UNSUPPORTED;
    return b->vt->write(b, buf, len, nwritten);
}

tc_status tc_backend_wait_ready(tc_backend* b, int32_t timeout_ms, bool* ready) {
    if (ready) *ready = false;
    if (!b || !b->vt || !b->vt->wait_ready) return TC_ERR_UNSUPPORTED;
    return b->vt->wait_ready(b, timeout_ms, ready);
}

tc_status tc_backend_read(tc_backend* b, void* buf, size_t cap, size_t* nread) {
    if (nread) *nread = 0;
    if (!b || !b->vt || !b->vt->read) return TC_ERR_UNSUPPORTED;
    return b->vt->read(b, buf, cap, nread);
}
