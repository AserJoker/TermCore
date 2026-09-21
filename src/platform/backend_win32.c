/* Windows backend: Console / ConPTY (docs/08).
 *
 * Console modes replace termios for raw mode, the console screen buffer
 * reports the size, and escape sequences are emitted through the same write
 * path once ENABLE_VIRTUAL_TERMINAL_PROCESSING is on (Windows 10+). Console
 * control events are not wired up yet; they land with the input layer.
 */
#include <platform/backend.h>

#if !defined(_WIN32)

tc_status tc_backend_create_win32(const tc_allocator* alloc, tc_backend** out) {
    (void)alloc;
    if (out) *out = NULL;
    return TC_ERR_UNSUPPORTED;
}

#else /* _WIN32 */

#if !defined(WIN32_LEAN_AND_MEAN)
#  define WIN32_LEAN_AND_MEAN
#endif
#include <string.h>
#include <windows.h>

#if !defined(ENABLE_VIRTUAL_TERMINAL_PROCESSING)
#  define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#if !defined(ENABLE_VIRTUAL_TERMINAL_INPUT)
#  define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

typedef struct win32_backend {
    tc_backend base;
    HANDLE     in;
    HANDLE     out;
    DWORD      saved_in;
    DWORD      saved_out;
    bool       saved_in_valid;
    bool       saved_out_valid;
    bool       raw;
    UINT       saved_out_cp;   /* console code pages, UTF-8 during the session */
    UINT       saved_in_cp;
    bool       out_cp_valid;
    bool       in_cp_valid;
} win32_backend;

static void win32_dispose(tc_backend* b) {
    win32_backend* w = (win32_backend*)b;

    if (w->raw) (void)b->vt->set_raw(b, false);
    if (w->saved_out_valid) (void)SetConsoleMode(w->out, w->saved_out);
    if (w->out_cp_valid) (void)SetConsoleOutputCP(w->saved_out_cp);
    if (w->in_cp_valid) (void)SetConsoleCP(w->saved_in_cp);
}

static tc_status win32_set_raw(tc_backend* b, bool on) {
    win32_backend* w = (win32_backend*)b;
    DWORD          in_mode  = 0;
    DWORD          out_mode = 0;
    bool           ok       = true;

    if (on == w->raw) return TC_OK;

    if (w->saved_in_valid) {
        in_mode = w->saved_in;
        if (on) {
            in_mode &= ~(DWORD)(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
            in_mode |= (DWORD)ENABLE_VIRTUAL_TERMINAL_INPUT;
        }
        ok &= (SetConsoleMode(w->in, in_mode) != 0);
    }

    /* Escape sequences only reach the terminal with VT processing enabled. */
    if (w->saved_out_valid) {
        out_mode = w->saved_out;
        if (on) out_mode |= (DWORD)(ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        ok &= (SetConsoleMode(w->out, out_mode) != 0);
    }

    if (!w->saved_in_valid && !w->saved_out_valid) return TC_ERR_UNSUPPORTED;
    if (!ok) return TC_ERR_BACKEND;

    w->raw = on;
    return TC_OK;
}

static tc_status win32_get_size(tc_backend* b, int32_t* cols, int32_t* rows) {
    win32_backend*             w    = (win32_backend*)b;
    CONSOLE_SCREEN_BUFFER_INFO info;

    if (!GetConsoleScreenBufferInfo(w->out, &info)) return TC_ERR_UNSUPPORTED;

    if (cols) *cols = (int32_t)(info.srWindow.Right - info.srWindow.Left + 1);
    if (rows) *rows = (int32_t)(info.srWindow.Bottom - info.srWindow.Top + 1);
    return TC_OK;
}

static tc_status win32_get_size_px(tc_backend* b, int32_t* w, int32_t* h) {
    (void)b;
    if (w) *w = 0;
    if (h) *h = 0;
    return TC_ERR_UNSUPPORTED;
}

static tc_status win32_set_cursor_visible(tc_backend* b, bool visible) {
    win32_backend*       w  = (win32_backend*)b;
    CONSOLE_CURSOR_INFO  ci;

    if (!GetConsoleCursorInfo(w->out, &ci)) return TC_ERR_UNSUPPORTED;

    ci.bVisible = visible ? TRUE : FALSE;
    return SetConsoleCursorInfo(w->out, &ci) ? TC_OK : TC_ERR_BACKEND;
}

static tc_status win32_set_cursor_pos(tc_backend* b, int32_t x, int32_t y) {
    win32_backend* w = (win32_backend*)b;
    COORD          c;

    c.X = (SHORT)x;
    c.Y = (SHORT)y;
    return SetConsoleCursorPosition(w->out, c) ? TC_OK : TC_ERR_BACKEND;
}

static tc_status win32_set_title(tc_backend* b, const char* utf8) {
    (void)b;
    if (!utf8) return TC_ERR_INVALID_ARG;
    return SetConsoleTitleA(utf8) ? TC_OK : TC_ERR_BACKEND;
}

static tc_status win32_write(tc_backend* b, const void* buf, size_t len, size_t* nwritten) {
    win32_backend* w     = (win32_backend*)b;
    const char*    bytes = (const char*)buf;
    size_t         done  = 0;

    if (nwritten) *nwritten = 0;

    while (done < len) {
        DWORD chunk   = (len - done > 0xFFFFu) ? 0xFFFFu : (DWORD)(len - done);
        DWORD written = 0;

        if (!WriteFile(w->out, bytes + done, chunk, &written, NULL)) {
            if (nwritten) *nwritten = done;
            return TC_ERR_IO;
        }
        if (written == 0) {   /* handle closed */
            if (nwritten) *nwritten = done;
            return TC_ERR_IO;
        }

        done += (size_t)written;
    }

    if (nwritten) *nwritten = done;
    return TC_OK;
}

/* The console input handle is waitable; park the caller until input arrives
 * or the timeout elapses (docs/04 §4.4). No busy loop. */
static tc_status win32_wait_ready(tc_backend* b, int32_t timeout_ms, bool* ready) {
    win32_backend* w = (win32_backend*)b;
    DWORD          ms;
    DWORD          rc;

    if (ready) *ready = false;
    if (!w->saved_in_valid) return TC_OK;   /* no console input */

    ms = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    rc = WaitForSingleObject(w->in, ms);
    if (rc == WAIT_OBJECT_0) {
        if (ready) *ready = true;
        return TC_OK;
    }
    if (rc == WAIT_TIMEOUT) return TC_OK;   /* ready stays false */
    return TC_ERR_IO;
}

/* Non-blocking read: probe the handle first so ReadFile never parks. nread=0
 * means "nothing right now", which terminates the greedy loop. */
static tc_status win32_read(tc_backend* b, void* buf, size_t cap, size_t* nread) {
    win32_backend* w = (win32_backend*)b;
    DWORD          n = 0;

    if (nread) *nread = 0;
    if (!w->saved_in_valid || cap == 0) return TC_OK;

    if (WaitForSingleObject(w->in, 0) != WAIT_OBJECT_0) return TC_OK;

    if (cap > 0xFFFF) cap = 0xFFFF;
    if (!ReadFile(w->in, buf, (DWORD)cap, &n, NULL)) {
        if (GetLastError() == ERROR_NO_DATA) return TC_OK;   /* raced */
        return TC_ERR_IO;
    }

    if (nread) *nread = (size_t)n;
    return TC_OK;
}

static const tc_backend_vtable g_win32_vtable = {
    win32_dispose,
    win32_set_raw,
    win32_get_size,
    win32_get_size_px,
    win32_set_cursor_visible,
    win32_set_cursor_pos,
    win32_set_title,
    win32_write,
    win32_wait_ready,
    win32_read
};

tc_status tc_backend_create_win32(const tc_allocator* alloc, tc_backend** out) {
    win32_backend* w;

    if (!out) return TC_ERR_INVALID_ARG;
    *out = NULL;
    if (!alloc) alloc = tc_allocator_default();

    w = (win32_backend*)alloc->alloc(alloc->ctx, sizeof(*w), sizeof(void*));
    if (!w) return TC_ERR_NOMEM;

    memset(w, 0, sizeof(*w));
    w->base.vt        = &g_win32_vtable;
    w->base.kind      = TC_BACKEND_WIN32;
    w->base.alloc     = alloc;
    w->base.self_size = sizeof(*w);
    w->in             = GetStdHandle(STD_INPUT_HANDLE);
    w->out            = GetStdHandle(STD_OUTPUT_HANDLE);

    if (w->in != INVALID_HANDLE_VALUE && w->in != NULL)
        w->saved_in_valid = (GetConsoleMode(w->in, &w->saved_in) != 0);
    if (w->out != INVALID_HANDLE_VALUE && w->out != NULL)
        w->saved_out_valid = (GetConsoleMode(w->out, &w->saved_out) != 0);

    /* Turn VT processing on up front: alternate screen, mouse and friends are
     * plain sequences for this backend too. */
    if (w->saved_out_valid) {
        (void)SetConsoleMode(w->out,
                             w->saved_out | (DWORD)(ENABLE_PROCESSED_OUTPUT |
                                                    ENABLE_VIRTUAL_TERMINAL_PROCESSING));
    }

    /* The library speaks UTF-8 (docs/08 §2.7): switch the console code pages
     * so the bytes we emit (and later read back) round-trip. The originals are
     * restored on dispose, like the console modes above. */
    if (w->saved_out_valid) {
        w->saved_out_cp = GetConsoleOutputCP();
        w->out_cp_valid = SetConsoleOutputCP(CP_UTF8) != 0;
    }
    if (w->saved_in_valid) {
        w->saved_in_cp = GetConsoleCP();
        w->in_cp_valid = SetConsoleCP(CP_UTF8) != 0;
    }

    *out = &w->base;
    return TC_OK;
}

#endif /* _WIN32 */
