/* Signal / console-event handling (docs/02 §9, §11).
 *
 * White-box: the internal signal module is exercised through the live-term
 * registry. Every case runs headless (null backend), so no terminal is touched
 * and the suite stays deterministic under ctest, where stdout is a pipe.
 */
#include <gtest/gtest.h>

#include <termcore/tc.h>

#include <control/signal_internal.h>
#include <control/term_internal.h>

namespace {

tc_term_options headless_options(const tc_allocator* alloc = nullptr) {
    tc_term_options opt;
    tc_init_term_options(&opt);
    opt.headless  = true;
    opt.allocator = alloc;
    return opt;
}

bool applied_snapshot(tc_term* t, bool* out) {
    for (int i = 0; i < TC_FEATURE_COUNT; i++) out[i] = t->applied[i];
    return t->cursor_hidden;
}

}  // namespace

/* The hooks are refcounted on the live-term count: the first create installs
 * them, the last destroy removes them (same pattern as the atexit hook). */
TEST(SignalInstall, FirstCreateInstallsLastDestroyUninstalls) {
    EXPECT_FALSE(tc_signal_is_installed());

    tc_term_options opt = headless_options();
    tc_term_t*      t1 = nullptr;
    ASSERT_EQ(tc_term_create(&opt, &t1), TC_OK);
    EXPECT_TRUE(tc_signal_is_installed());

    tc_term_t* t2 = nullptr;
    ASSERT_EQ(tc_term_create(&opt, &t2), TC_OK);
    EXPECT_TRUE(tc_signal_is_installed());

    tc_term_destroy(t1);
    EXPECT_TRUE(tc_signal_is_installed());   /* one live term left */

    tc_term_destroy(t2);
    EXPECT_FALSE(tc_signal_is_installed());
}

TEST(SignalFlags, ResizeIsSetThenTakenExactlyOnce) {
    EXPECT_FALSE(tc_signal_take_resize());
    tc_signal_note_resize();
    EXPECT_TRUE(tc_signal_take_resize());
    EXPECT_FALSE(tc_signal_take_resize());   /* one-shot */
}

TEST(SignalFlags, ResumeFlagStartsCleared) {
    EXPECT_FALSE(tc_signal_take_resume());
    EXPECT_FALSE(tc_signal_take_resume());
}

/* The signal path may only write closing sequences and restore raw mode; the
 * state machine and applied[] are owned by the normal control flow (docs/02
 * §9). After restore_all an ACTIVE session must still look ACTIVE with its
 * applied bits intact, and a subsequent leave() must stay well-formed. */
TEST(SignalRestore, RestoreAllLeavesTheStateMachineUntouched) {
    tc_term_options opt = headless_options();
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);
    ASSERT_EQ(tc_term_enter(term), TC_OK);

    tc_term* m = reinterpret_cast<tc_term*>(term);
    EXPECT_EQ(m->state, TC_TERM_STATE_ACTIVE);

    bool before[TC_FEATURE_COUNT];
    const bool before_hidden = applied_snapshot(m, before);

    tc_signal_restore_all();

    EXPECT_EQ(m->state, TC_TERM_STATE_ACTIVE);
    EXPECT_EQ(m->cursor_hidden, before_hidden);
    for (int i = 0; i < TC_FEATURE_COUNT; i++) {
        EXPECT_EQ(m->applied[i], before[i]) << "applied[" << i << "] mutated";
    }

    /* The normal path still works afterwards and clears the state cleanly. */
    EXPECT_EQ(tc_term_leave(term), TC_OK);
    EXPECT_EQ(m->state, TC_TERM_STATE_CREATED);

    tc_term_destroy(term);
}

/* After a signal restore the terminal is physically back to normal while the
 * session still reports ACTIVE; reenter() must re-apply every feature from
 * normal control flow (docs/02 §9). */
TEST(SignalRestore, ReenterReappliesFeaturesAfterSignalRestore) {
    tc_term_options opt = headless_options();
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);
    ASSERT_EQ(tc_term_enter(term), TC_OK);

    tc_term* m = reinterpret_cast<tc_term*>(term);
    tc_signal_restore_all();   /* simulate the SIGTSTP path */
    EXPECT_EQ(m->state, TC_TERM_STATE_ACTIVE);

    ASSERT_EQ(tc_term_reenter(term), TC_OK);
    EXPECT_EQ(m->state, TC_TERM_STATE_ACTIVE);
    EXPECT_TRUE(m->applied[TC_FEATURE_RAW_MODE]);
    EXPECT_TRUE(m->applied[TC_FEATURE_ALT_SCREEN]);
    EXPECT_TRUE(m->applied[TC_FEATURE_CAPTURE_CTRL_C]);
    EXPECT_TRUE(m->cursor_hidden);

    tc_term_destroy(term);
}

TEST(SignalRestore, ReenterOnAnInactiveSessionSimplyEnters) {
    tc_term_options opt = headless_options();
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);
    ASSERT_EQ(tc_term_reenter(term), TC_OK);

    tc_term* m = reinterpret_cast<tc_term*>(term);
    EXPECT_EQ(m->state, TC_TERM_STATE_ACTIVE);

    EXPECT_EQ(tc_term_reenter(nullptr), TC_ERR_INVALID_ARG);

    tc_term_destroy(term);
}

#if !defined(_WIN32)

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* The TERM handler is installed for as long as any live term exists and the
 * previous disposition is restored on uninstall. */
TEST(SignalPosix, TermHandlerIsInstalledThenRestored) {
    EXPECT_FALSE(tc_signal_is_installed());

    struct sigaction sa;
    ASSERT_EQ(sigaction(SIGTERM, nullptr, &sa), 0);
    EXPECT_EQ(sa.sa_handler, SIG_DFL);   /* pristine test process */

    {
        tc_term_options opt = headless_options();
        tc_term_t*      term = nullptr;
        ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);
        ASSERT_TRUE(tc_signal_is_installed());

        ASSERT_EQ(sigaction(SIGTERM, nullptr, &sa), 0);
        EXPECT_NE(sa.sa_handler, SIG_DFL);

        tc_term_destroy(term);
    }

    EXPECT_FALSE(tc_signal_is_installed());
    ASSERT_EQ(sigaction(SIGTERM, nullptr, &sa), 0);
    EXPECT_EQ(sa.sa_handler, SIG_DFL);   /* previous disposition restored */
}

/* End to end: an ACTIVE session is restored and the process dies by the
 * original signal, so waitpid(2) observes 128+sig semantics. The child uses
 * _exit on any setup failure so the parent sees a distinguishable status. */
TEST(SignalPosix, TermSignalRestoresAndDiesWithTheSignal) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        tc_term_options opt;
        tc_init_term_options(&opt);
        opt.headless = true;

        tc_term_t* term = nullptr;
        if (tc_term_create(&opt, &term) != TC_OK) _exit(41);
        if (tc_term_enter(term) != TC_OK) _exit(42);

        struct sigaction sa;
        if (sigaction(SIGTERM, nullptr, &sa) != 0) _exit(43);
        if (sa.sa_handler == SIG_DFL) _exit(44);   /* handler not installed */

        raise(SIGTERM);
        _exit(45);   /* the handler must not return */
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);

    /* The handler dies with the conventional 128+sig status (docs/02 §9).
     * raise() inside the handler is often deferred because the signal is
     * implicitly blocked for its own handler, so the _exit(128+sig) fallback
     * runs and waitpid sees a normal exit; when SA_NODEFER-style delivery
     * wins instead, waitpid sees the signal death. Both are correct. */
    const bool died_by_signal = WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM;
    const bool died_with_128  = WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGTERM;
    EXPECT_TRUE(died_by_signal || died_with_128)
        << "raw status 0x" << std::hex << status << std::dec;
}

#endif /* !_WIN32 */
