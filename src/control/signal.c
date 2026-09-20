/* Signal / console-event handling (docs/02 §9, §11).
 *
 * POSIX: sigaction-based handlers. The handler body is restricted to
 *   - writing volatile sig_atomic_t flags,
 *   - the async-signal-safe restore (compile-time sequences + tcsetattr),
 *   - restoring the default disposition and re-raising a terminating signal.
 *
 * Windows: SetConsoleCtrlHandler runs the callback on a dedicated thread, so
 * the async-signal-safe constraint does not apply there; the same restore
 * function is used for consistency.
 *
 * Installation is refcounted by term.c: first create installs, last destroy
 * uninstalls and restores the previous handlers.
 *
 * The feature-test macro must precede every include: strict C11 (-std=c11)
 * hides `struct sigaction` behind it, and glibc's features.h snapshots the
 * __USE_* set on first sight.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <control/signal_internal.h>

#include <signal.h>
#include <string.h>

#if defined(_WIN32)

/* ---------------------------------------------------------------------------
 * Windows: console control events
 * ------------------------------------------------------------------------- */
#include <windows.h>

static bool                      g_installed = false;
static volatile sig_atomic_t     g_resize_pending = 0;
static volatile sig_atomic_t     g_resume_pending = 0;

static BOOL WINAPI tc_console_handler(DWORD evt) {
    switch (evt) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        /* Restore first, then let the system default action run. Returning
         * TRUE would swallow the event: the process would neither terminate
         * nor see the key (no KEY_EVENT is queued for a signal), which is
         * what makes Ctrl+C appear dead. With ENABLE_PROCESSED_INPUT on (raw
         * mode not in effect) the conventional Ctrl+C/Ctrl+Break termination
         * applies, mirroring the POSIX branch where SIGINT is left to its
         * default disposition. */
        tc_signal_restore_all();
        return FALSE;
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        /* Best effort: the console is going away, the process exits shortly
         * regardless of the return value. */
        tc_signal_restore_all();
        return TRUE;
    default:
        return FALSE;   /* let the next handler / default action run */
    }
}

tc_status tc_signal_install(void) {
    if (g_installed) return TC_OK;

    /* Handlers are chained, not stacked: ours is added at the front and the
     * console invokes each one in order until one returns TRUE. Nothing is
     * overwritten, so uninstall only has to remove ours again. */
    if (!SetConsoleCtrlHandler(tc_console_handler, TRUE)) return TC_ERR_BACKEND;

    g_installed = true;
    return TC_OK;
}

void tc_signal_uninstall(void) {
    if (!g_installed) return;
    (void)SetConsoleCtrlHandler(tc_console_handler, FALSE);
    g_installed = false;
}

bool tc_signal_is_installed(void) {
    return g_installed;
}

void tc_signal_note_resize(void) {
    g_resize_pending = 1;
}

bool tc_signal_take_resize(void) {
    if (!g_resize_pending) return false;
    g_resize_pending = 0;
    return true;
}

bool tc_signal_take_resume(void) {
    if (!g_resume_pending) return false;
    g_resume_pending = 0;
    return true;
}

#else /* !_WIN32 */

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t g_resize_pending  = 0;
static volatile sig_atomic_t g_resume_pending  = 0;
static bool                  g_installed       = false;

/* Original dispositions, restored on uninstall. */
static struct sigaction g_old_winch;
static struct sigaction g_old_term;
static struct sigaction g_old_hup;
static struct sigaction g_old_tstp;
static struct sigaction g_old_cont;
static struct sigaction g_old_fatal[6];   /* SEGV ABRT BUS FPE ILL QUIT */
static int              g_fatal_sigs[6];

static void handle_winch(int sig) {
    (void)sig;
    g_resize_pending = 1;   /* docs/02 §8: flag only, refresh from normal flow */
}

static void handle_term(int sig) {
    /* Restore the terminal, then die with the conventional code so that
     * waitpid(2) observes 128+sig (docs/02 §9). */
    tc_signal_restore_all();
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);   /* in case the default disposition is blocked */
}

static void handle_tstp(int sig) {
    /* Suspend the whole foreground job like the shell does: restore the
     * terminal first, then really stop. On SIGCONT we are woken and the
     * original disposition must be ours again (docs/02 §9). */
    (void)sig;
    tc_signal_restore_all();
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
    signal(SIGTSTP, handle_tstp);
}

static void handle_cont(int sig) {
    (void)sig;
    g_resume_pending = 1;   /* re-enter is up to the normal control flow */
}

static void handle_fatal(int sig) {
    /* Best effort (docs/02 §11): restore, then re-raise so core dumps and
     * the debugger still see the original signal. */
    tc_signal_restore_all();
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

static void install_one(int sig, void (*handler)(int), struct sigaction* old) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, old);
}

tc_status tc_signal_install(void) {
    int i = 0;

    if (g_installed) return TC_OK;

    install_one(SIGWINCH, handle_winch, &g_old_winch);
    install_one(SIGTERM,  handle_term,  &g_old_term);
    install_one(SIGHUP,   handle_term,  &g_old_hup);
    install_one(SIGTSTP,  handle_tstp,  &g_old_tstp);
    install_one(SIGCONT,  handle_cont,  &g_old_cont);
    /* SIGINT stays untouched while capture_ctrl_c is on: the input layer
     * turns the byte into an event (docs/02 §9). When it is off, raw mode
     * still clears ISIG, so the process would not see a signal at all;
     * nothing to do here. */

    g_fatal_sigs[i++] = SIGSEGV;
    g_fatal_sigs[i++] = SIGABRT;
    g_fatal_sigs[i++] = SIGBUS;
    g_fatal_sigs[i++] = SIGFPE;
    g_fatal_sigs[i++] = SIGILL;
    g_fatal_sigs[i++] = SIGQUIT;

    for (i = 0; i < 6; i++) install_one(g_fatal_sigs[i], handle_fatal, &g_old_fatal[i]);

    g_installed = true;
    return TC_OK;
}

void tc_signal_uninstall(void) {
    int i;

    if (!g_installed) return;

    sigaction(SIGWINCH, &g_old_winch, NULL);
    sigaction(SIGTERM,  &g_old_term,  NULL);
    sigaction(SIGHUP,   &g_old_hup,   NULL);
    sigaction(SIGTSTP,  &g_old_tstp,  NULL);
    sigaction(SIGCONT,  &g_old_cont,  NULL);

    for (i = 0; i < 6; i++) sigaction(g_fatal_sigs[i], &g_old_fatal[i], NULL);

    g_installed = false;
}

bool tc_signal_is_installed(void) {
    return g_installed;
}

void tc_signal_note_resize(void) {
    g_resize_pending = 1;
}

bool tc_signal_take_resize(void) {
    if (!g_resize_pending) return false;
    g_resize_pending = 0;
    return true;
}

bool tc_signal_take_resume(void) {
    if (!g_resume_pending) return false;
    g_resume_pending = 0;
    return true;
}

#endif /* _WIN32 */

/* ---------------------------------------------------------------------------
 * Shared: async-signal-safe terminal restore (docs/02 §11)
 * ------------------------------------------------------------------------- */
void tc_signal_restore_all(void) {
    const tc_term* t = tc_term_live_first();
    while (t) {
        /* Only sessions that actually entered the TUI state need restoring;
         * the dedicated restore path never mutates state/applied, so the
         * normal control flow stays consistent (docs/02 §9). */
        if (t->state == TC_TERM_STATE_ACTIVE) tc_term_restore_signal((tc_term*)t);
        t = tc_term_live_next(t);
    }
}
