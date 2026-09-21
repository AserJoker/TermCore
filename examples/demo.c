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
#include <string.h>

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
static void sleep_ms(int ms) {
    Sleep((DWORD)ms);
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
static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000,
                          .tv_nsec = (long)(ms % 1000) * 1000 * 1000L};
    (void)nanosleep(&ts, NULL);
}
#endif

static void sleep_short(void) {
    sleep_ms(50);
}

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

/* --------------------------------------------------------------------------
 * Render layer demo (docs/06): the single-surface model. Every frame is drawn
 * into an off-screen cell grid; tc_present diffs it against the previous frame
 * and emits only the changed runs (docs/06 §9). On a real terminal the frames
 * are visible inside the session; headless runs exercise the same code path
 * into the null backend and are what CI runs.
 * ------------------------------------------------------------------------ */

/* Draws the demo frame. frame_two changes only the status line, so the second
 * present re-emits a single run instead of the whole grid. */
static void draw_frame(tc_surface_t* s, bool frame_two) {
    tc_style_attr a;
    int32_t       cols = 0;
    int32_t       rows = 0;

    (void)tc_surface_get_size(s, &cols, &rows);

    memset(&a, 0, sizeof(a));
    a.fg    = tc_color_rgb(255, 200, 0);
    a.bg    = tc_color_indexed(17);
    a.style = TC_STYLE_BOLD;
    (void)tc_surface_draw_text(s, 0, 0, " TermCore render demo ",
                               &a, TC_TEXT_UNICODE, NULL);

    memset(&a, 0, sizeof(a));
    (void)tc_surface_draw_text(s, 0, 2,
                               "Single-surface model: draw into cells, tc_present diffs.",
                               &a, TC_TEXT_UNICODE, NULL);

    memset(&a, 0, sizeof(a));
    a.style = TC_STYLE_BOLD | TC_STYLE_UNDERLINE | TC_STYLE_ITALIC;
    (void)tc_surface_draw_text(s, 0, 3, "bold + underline + italic",
                               &a, TC_TEXT_UNICODE, NULL);

    memset(&a, 0, sizeof(a));
    a.fg = tc_color_rgb(255, 64, 64);
    a.bg = tc_color_rgb(32, 32, 64);
    (void)tc_surface_draw_text(s, 0, 4, "truecolor  fg 255;64;64  bg 32;32;64",
                               &a, TC_TEXT_UNICODE, NULL);

    memset(&a, 0, sizeof(a));
    a.fg = tc_color_indexed(208);
    (void)tc_surface_draw_text(s, 0, 5, "indexed fg 208 (orange)",
                               &a, TC_TEXT_UNICODE, NULL);

    memset(&a, 0, sizeof(a));
    (void)tc_surface_draw_text(s, 0, 6,
                               "wide: \xe4\xb8\xad\xe6\x96\x87 CJK double-width",
                               &a, TC_TEXT_UNICODE, NULL);

    /* Status line at the bottom: the only cells the second frame changes. */
    if (rows > 0) {
        char line[64];
        memset(&a, 0, sizeof(a));
        a.fg    = frame_two ? tc_color_rgb(0, 255, 128)
                            : tc_color_rgb(255, 255, 255);
        a.style = frame_two ? TC_STYLE_BOLD : TC_STYLE_NONE;
        (void)snprintf(line, sizeof(line), "status: %s",
                       frame_two ? "done" : "idle");
        (void)tc_surface_draw_text(s, 0, rows - 1, line, &a,
                                   TC_TEXT_UNICODE, NULL);
    }
}

static int run_render(bool headless) {
    tc_term_options opt;
    tc_term_t*      term    = NULL;
    tc_surface_t*   surface = NULL;
    tc_status       st;
    int32_t         cols    = 0;
    int32_t         rows    = 0;

    tc_init_term_options(&opt);
    opt.headless = headless;

    st = tc_term_create(&opt, &term);
    if (st != TC_OK) {
        printf("render: tc_term_create: %s\n", tc_status_string(st));
        return 1;
    }

    /* Headless sessions cannot ask the terminal, so the size is pinned. */
    if (headless) (void)tc_term_set_size(term, 80, 24);

    st = tc_term_enter(term);
    if (st != TC_OK) {
        printf("render: tc_term_enter: %s\n", tc_status_string(st));
        tc_term_destroy(term);
        return 1;
    }

    (void)tc_term_get_size(term, &cols, &rows);
    if (cols > 0 && rows > 0) {
        st = tc_surface_create(term, cols, rows, &surface);
        if (st != TC_OK) {
            printf("render: tc_surface_create: %s\n", tc_status_string(st));
        } else {
            draw_frame(surface, false);
            (void)tc_present(term, surface);

            /* Pause briefly so the first frame is visible before the diff. */
            if (!headless) sleep_ms(1000);

            draw_frame(surface, true);
            (void)tc_present(term, surface);

            if (!headless && hold_requested()) {
                int key;
                while ((key = read_key()) != 'q') {
                    if (key >= 0) {
                        printf("\r         key 0x%02x ignored, press 'q' to leave\r",
                               key);
                        fflush(stdout);
                    }
                    sleep_short();
                }
                printf("\r\n");
            }

            tc_surface_destroy(surface);
            surface = NULL;
        }
    }

    st = tc_term_leave(term);
    if (st != TC_OK) printf("render: tc_term_leave: %s\n", tc_status_string(st));

    tc_term_destroy(term);
    printf("render: %dx%d, two frames presented%s\n", cols, rows,
           headless ? " (headless)" : "");
    return 0;
}

int main(void) {
    const bool interactive = tc_stdout_is_tty();

    print_info();
    if (allocator_round_trip() != 0) return 1;

    if (interactive) {
        if (run_session(false) != 0) return 1;
        if (run_render(false) != 0) return 1;
    } else {
        printf("\nstdout is not a terminal: running headless.\n");
    }

    if (run_session(true) != 0) return 1;
    return run_render(true);
}
