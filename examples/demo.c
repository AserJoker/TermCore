/* Combined demo: version, platform probe, allocator round-trip and a full
 * terminal session (docs/02 §2, §3). Later layers (caps / input / render)
 * get their own sections here as they land (docs/10 §5).
 *
 * Build: cmake --preset windows-clang && cmake --build --preset windows-clang
 * Run:   build/windows-clang/bin/termcore_demo
 *
 * The session part runs on a real terminal when stdout is one; otherwise it
 * falls back to headless, which is exactly the path CI and pipes take.
 */
/* Strict C11 (-std=c11) hides POSIX declarations like nanosleep behind the
 * feature-test macros; this must precede every include (glibc snapshots the
 * __USE_* set on first sight). Windows ignores it. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>

#include <termcore/tc.h>

/* TERMCORE_DEMO_HOLD=1 keeps a real session active until 'q' is pressed, so
 * the alt screen, mouse reporting and hidden cursor can be inspected in an
 * external window. Headless sessions (and therefore CI) never wait. */
static bool hold_requested(void) {
    const char* v = getenv("TERMCORE_DEMO_HOLD");
    return v != NULL && v[0] != '\0' && v[0] != '0';
}

/* Input polling for the hold loop. Raw mode is on, so every byte arrives as it
 * is typed; the loop must not busy-wait, hence the short sleep per iteration.
 * This is deliberately a stand-in for the input layer (docs/04) that is not
 * implemented yet. */
#if defined(_WIN32)
#include <conio.h>
#include <windows.h>
static int read_key(void) {
    return _kbhit() ? _getch() : -1;   /* -1: nothing waiting */
}
static void sleep_short(void) {
    Sleep(50);
}
#else
#include <poll.h>
#include <time.h>
#include <unistd.h>
static int read_key(void) {
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&pfd, 1, 0) <= 0) return -1;   /* -1: nothing waiting */
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return (int)c;
}
static void sleep_short(void) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
    (void)nanosleep(&ts, NULL);
}
#endif

static void print_info(void) {
    tc_version v;
    tc_version_get(&v);

    printf("termcore %s (abi %d)\n", v.string, v.abi);
    printf("platform: %s\n", tc_platform_name(tc_platform_current()));
    printf("backend : %d\n", (int)tc_platform_default_backend());
    printf("stdout tty: %s\n", tc_stdout_is_tty() ? "yes" : "no");
}

static int allocator_round_trip(void) {
    const tc_allocator* a = tc_allocator_default();
    void* p = a->alloc(a->ctx, 32, 8);

    if (!p) {
        printf("alloc failed: %s\n", tc_status_string(TC_ERR_NOMEM));
        return 1;
    }

    a->free(a->ctx, p, 32);
    printf("allocator round-trip: ok\n");
    return 0;
}

/* One full session: create (does not touch the terminal) -> enter -> leave
 * (idempotent) -> destroy (leaves first). */
static int run_session(bool headless) {
    tc_term_options opt;
    tc_term_t*      term   = NULL;
    tc_status       st;
    int32_t         cols   = 0;
    int32_t         rows   = 0;
    bool            active = false;

    tc_init_term_options(&opt);
    opt.headless     = headless;
    opt.mouse_mode    = TC_MOUSE_CLICK;   /* 1000 + SGR pixels */
    opt.focus_events  = true;

    st = tc_term_create(&opt, &term);
    if (st != TC_OK) {
        printf("tc_term_create: %s\n", tc_status_string(st));
        return 1;
    }

    (void)tc_term_is_active(term, &active);
    printf("\nsession (headless=%d): created, active=%d\n", (int)headless, (int)active);

    /* Headless has no terminal to ask, so tests and CI pin the size instead. */
    if (headless) (void)tc_term_set_size(term, 120, 40);

    st = tc_term_enter(term);
    if (st != TC_OK) {
        printf("tc_term_enter: %s\n", tc_status_string(st));
        tc_term_destroy(term);
        return 1;
    }
    (void)tc_term_is_active(term, &active);

    (void)tc_term_get_size(term, &cols, &rows);

    if (!headless && hold_requested()) {
        /* Still active: alt screen on, cursor hidden, mouse reporting armed.
         * Raw mode means a bare \n does not move to column 0, hence \r\n.
         *
         * Poll stdin briefly and sleep: only 'q' leaves the session, any other
         * key is echoed back and the loop continues. */
        printf("\r\n[active] alt screen + raw mode, cursor hidden\r\n"
               "         press 'q' to leave\r\n");
        fflush(stdout);

        int key;
        while ((key = read_key()) != 'q') {
            if (key >= 0) {
                printf("\r         key 0x%02x ignored, press 'q' to leave\r", key);
                fflush(stdout);
            }
            sleep_short();
        }
        printf("\r\n");
    }

    /* Raw mode is on from here until leave(): OPOST is off, so a newline no
     * longer implies a carriage return. Real applications write through the
     * render layer while active and use printf only outside the session. */
    st = tc_term_leave(term);
    if (st != TC_OK) printf("tc_term_leave: %s\n", tc_status_string(st));

    printf("entered, then left: active was %d, size %dx%d\n", (int)active, cols, rows);

    (void)tc_term_leave(term);   /* idempotent */
    tc_term_destroy(term);       /* leaves again, then releases */
    printf("destroyed\n");

    return 0;
}

int main(void) {
    const bool interactive = tc_stdout_is_tty();

    print_info();
    if (allocator_round_trip() != 0) return 1;

    if (interactive) {
        if (run_session(false) != 0) return 1;
    } else {
        printf("\nstdout is not a terminal: running the session headless.\n");
    }

    return run_session(true);
}
