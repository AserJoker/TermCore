/* POSIX backend: Linux / macOS / BSD (docs/08 §2).
 *
 * termios for raw mode, ioctl(TIOCGWINSZ) for the size, write() for output.
 * Signals are NOT installed here yet; they land with the input layer, which
 * owns the event queue they feed (docs/02 §9).
 */
#if !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE 1
#endif

#include <platform/backend.h>

#if defined(_WIN32)

tc_status tc_backend_create_posix(const tc_allocator* alloc, tc_backend** out) {
    (void)alloc;
    if (out) *out = NULL;
    return TC_ERR_UNSUPPORTED;
}

#else /* !_WIN32 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

typedef struct posix_backend {
    tc_backend     base;
    int            in_fd;
    int            out_fd;
    bool           owns_in;         /* /dev/tty was opened by us */
    struct termios saved_termios;
    bool           saved_valid;
    bool           raw;
} posix_backend;

static void posix_dispose(tc_backend* b) {
    posix_backend* p = (posix_backend*)b;

    /* Best effort: a session destroyed without leave() must still restore. */
    if (p->raw) (void)b->vt->set_raw(b, false);
    if (p->owns_in && p->in_fd >= 0) close(p->in_fd);
    p->in_fd = -1;
}

/* Hand-written, because cfmakeraw is not standard (docs/08 §2.2). */
static tc_status posix_set_raw(tc_backend* b, bool on) {
    posix_backend* p = (posix_backend*)b;

    if (on == p->raw) return TC_OK;

    if (on) {
        struct termios t;

        if (tcgetattr(p->in_fd, &t) != 0) return TC_ERR_BACKEND;

        p->saved_termios = t;
        p->saved_valid   = true;

        t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        t.c_oflag &= ~(tcflag_t)OPOST;
        t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
        t.c_cflag |= (tcflag_t)CS8;
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;

        /* TCSAFLUSH drops input typed before we took over. */
        if (tcsetattr(p->in_fd, TCSAFLUSH, &t) != 0) return TC_ERR_BACKEND;

        p->raw = true;
        return TC_OK;
    }

    if (p->saved_valid && tcsetattr(p->in_fd, TCSANOW, &p->saved_termios) != 0)
        return TC_ERR_BACKEND;

    p->raw = false;
    return TC_OK;
}

static bool env_size(const char* name, int32_t* out) {
    const char* text = getenv(name);
    char*       end  = NULL;
    long        v;

    if (!text || !*text) return false;

    v = strtol(text, &end, 10);
    if (end == text || v <= 0 || v > 65535) return false;

    *out = (int32_t)v;
    return true;
}

static tc_status posix_get_size(tc_backend* b, int32_t* cols, int32_t* rows) {
    posix_backend* p = (posix_backend*)b;
    struct winsize ws;
    int32_t        c = 0;
    int32_t        r = 0;

    memset(&ws, 0, sizeof(ws));
    if (ioctl(p->out_fd, TIOCGWINSZ, &ws) != 0)
        (void)ioctl(p->in_fd, TIOCGWINSZ, &ws);

    if (ws.ws_col > 0 && ws.ws_row > 0) {
        if (cols) *cols = (int32_t)ws.ws_col;
        if (rows) *rows = (int32_t)ws.ws_row;
        return TC_OK;
    }

    /* Containers and CI often have neither a tty nor SIGWINCH (docs/08 §2.5). */
    if (env_size("COLUMNS", &c) && env_size("LINES", &r)) {
        if (cols) *cols = c;
        if (rows) *rows = r;
        return TC_OK;
    }

    return TC_ERR_UNSUPPORTED;
}

static tc_status posix_get_size_px(tc_backend* b, int32_t* w, int32_t* h) {
    posix_backend* p = (posix_backend*)b;
    struct winsize ws;

    if (w) *w = 0;
    if (h) *h = 0;

    memset(&ws, 0, sizeof(ws));
    if (ioctl(p->out_fd, TIOCGWINSZ, &ws) != 0)
        (void)ioctl(p->in_fd, TIOCGWINSZ, &ws);

    /* ws_xpixel / ws_ypixel are 0 on most terminals; only report what we got. */
    if (w) *w = (int32_t)ws.ws_xpixel;
    if (h) *h = (int32_t)ws.ws_ypixel;
    return (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) ? TC_OK : TC_ERR_UNSUPPORTED;
}

static tc_status posix_set_cursor_visible(tc_backend* b, bool visible) {
    const char* seq = visible ? "\x1b[?25h" : "\x1b[?25l";
    return b->vt->write(b, seq, strlen(seq), NULL);
}

static tc_status posix_set_cursor_pos(tc_backend* b, int32_t x, int32_t y) {
    char buf[32];
    int  n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (int)(y + 1), (int)(x + 1));
    if (n < 0 || (size_t)n >= sizeof(buf)) return TC_ERR_INVALID_ARG;
    return b->vt->write(b, buf, (size_t)n, NULL);
}

static tc_status posix_set_title(tc_backend* b, const char* utf8) {
    tc_status st;

    if (!utf8) return TC_ERR_INVALID_ARG;

    st = b->vt->write(b, "\x1b]0;", 4, NULL);
    if (st != TC_OK) return st;
    st = b->vt->write(b, utf8, strlen(utf8), NULL);
    if (st != TC_OK) return st;
    return b->vt->write(b, "\x07", 1, NULL);
}

static tc_status posix_write(tc_backend* b, const void* buf, size_t len, size_t* nwritten) {
    posix_backend* p     = (posix_backend*)b;
    const char*    bytes = (const char*)buf;
    size_t         done  = 0;

    if (nwritten) *nwritten = 0;

    while (done < len) {
        ssize_t n = write(p->out_fd, bytes + done, len - done);

        if (n < 0) {
            if (errno == EINTR) continue;
            if (nwritten) *nwritten = done;
            return TC_ERR_IO;
        }
        if (n == 0) {   /* terminal went away */
            if (nwritten) *nwritten = done;
            return TC_ERR_IO;
        }

        done += (size_t)n;
    }

    if (nwritten) *nwritten = done;
    return TC_OK;
}

static const tc_backend_vtable g_posix_vtable = {
    posix_dispose,
    posix_set_raw,
    posix_get_size,
    posix_get_size_px,
    posix_set_cursor_visible,
    posix_set_cursor_pos,
    posix_set_title,
    posix_write
};

tc_status tc_backend_create_posix(const tc_allocator* alloc, tc_backend** out) {
    posix_backend* p;
    int            fd;

    if (!out) return TC_ERR_INVALID_ARG;
    *out = NULL;
    if (!alloc) alloc = tc_allocator_default();

    p = (posix_backend*)alloc->alloc(alloc->ctx, sizeof(*p), sizeof(void*));
    if (!p) return TC_ERR_NOMEM;

    memset(p, 0, sizeof(*p));
    p->base.vt        = &g_posix_vtable;
    p->base.kind      = TC_BACKEND_POSIX;
    p->base.alloc     = alloc;
    p->base.self_size = sizeof(*p);
    p->out_fd         = STDOUT_FILENO;

    /* /dev/tty keeps the keyboard readable even when stdin is redirected
     * (docs/08 §2.1); stdin is the fallback. */
    fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        p->in_fd   = fd;
        p->owns_in = true;
    } else {
        p->in_fd   = STDIN_FILENO;
        p->owns_in = false;
    }

    *out = &p->base;
    return TC_OK;
}

#endif /* _WIN32 */
