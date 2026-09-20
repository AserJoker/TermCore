#ifndef TERMCORE_CONTROL_SIGNAL_INTERNAL_H
#define TERMCORE_CONTROL_SIGNAL_INTERNAL_H

/* Signal / console-event handling for the control layer (docs/02 §9, §11).
 *
 * The signal handler may only write a volatile sig_atomic_t flag and perform
 * the minimal async-signal-safe terminal restore: every sequence below is a
 * compile-time constant and no allocation happens on this path (docs/02 §11).
 *
 * Install/uninstall are refcounted: the first live term installs the hooks,
 * the last one removes them (same pattern as the atexit hook in term.c).
 */

#include <control/term_internal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* term.c calls these. First install / last uninstall; TC_OK on success. */
tc_status tc_signal_install(void);
void      tc_signal_uninstall(void);

/* Test-visible: whether the signal hooks are currently installed. */
bool tc_signal_is_installed(void);

/* Restores every ACTIVE live term without touching its state machine
 * (best effort, async-signal-safe). Used by TERM/HUP/CTRL_CLOSE and the
 * fatal-signal handlers before the process goes away. */
void tc_signal_restore_all(void);

/* SIGWINCH / console resize notification: only sets a flag (docs/02 §8, §9).
 * The normal control flow polls/consumes it via take_resize(). */
void tc_signal_note_resize(void);
bool tc_signal_take_resize(void);

/* SIGCONT resume notification: only sets a flag. The application re-enters
 * the session from normal control flow (tc_term_reenter). */
bool tc_signal_take_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_CONTROL_SIGNAL_INTERNAL_H */
