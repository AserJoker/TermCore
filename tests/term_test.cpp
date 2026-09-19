/* Control layer: session lifecycle (docs/02 §3).
 *
 * Every case runs headless: no terminal is touched, so the suite is
 * deterministic under ctest, where stdout is a pipe. */
#include <gtest/gtest.h>

#include <termcore/tc.h>

namespace {

tc_term_options headless_options(const tc_allocator* alloc = nullptr) {
    tc_term_options opt;
    tc_init_term_options(&opt);
    opt.headless  = true;
    opt.allocator = alloc;
    return opt;
}

}  // namespace

TEST(TermOptions, DefaultsMatchTheDocumentedContract) {
    tc_term_options opt;
    tc_init_term_options(&opt);

    EXPECT_TRUE(opt.raw_mode);
    EXPECT_TRUE(opt.alternate_screen);
    EXPECT_EQ(opt.mouse_mode, TC_MOUSE_OFF);
    EXPECT_FALSE(opt.focus_events);
    EXPECT_TRUE(opt.bracketed_paste);
    EXPECT_TRUE(opt.kitty_keyboard);
    EXPECT_TRUE(opt.hide_cursor);
    EXPECT_TRUE(opt.capture_ctrl_c);
    EXPECT_TRUE(opt.sync_update);

    EXPECT_FALSE(opt.headless);
    EXPECT_TRUE(opt.query_capabilities);
    EXPECT_EQ(opt.caps_profile, nullptr);
    EXPECT_FALSE(opt.strict_diff);
    EXPECT_EQ(opt.text_mode, TC_TEXT_UNICODE);
    EXPECT_FALSE(opt.ambiguous_wide);

    EXPECT_EQ(opt.capability_timeout_ms, 200);
    EXPECT_EQ(opt.esc_timeout_ms, 100);
    EXPECT_EQ(opt.read_batch_max_bytes, 4096);
    EXPECT_EQ(opt.event_queue_capacity, 256);
    EXPECT_EQ(opt.overflow, TC_OVERFLOW_DROP_OLDEST);
    EXPECT_EQ(opt.allocator, nullptr);
}

TEST(TermOptions, InitAcceptsNull) {
    tc_init_term_options(nullptr);   /* must not crash */
}

TEST(TermLifecycle, CreateYieldsAnInactiveSession) {
    tc_term_options opt = headless_options();
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);
    ASSERT_NE(term, nullptr);

    bool active = true;
    ASSERT_EQ(tc_term_is_active(term, &active), TC_OK);
    EXPECT_FALSE(active);

    tc_term_destroy(term);
}

TEST(TermLifecycle, NullOptionsMeansAllDefaults) {
    /* Defaults are not headless, so this only succeeds on a real terminal. */
    tc_term_t* term = nullptr;
    const tc_status st = tc_term_create(nullptr, &term);

    if (tc_stdout_is_tty()) {
        EXPECT_EQ(st, TC_OK);
        tc_term_destroy(term);
    } else {
        EXPECT_EQ(st, TC_ERR_NOT_A_TTY);
        EXPECT_EQ(term, nullptr);
    }
}

TEST(TermLifecycle, NonTtyWithoutHeadlessIsRejected) {
    if (tc_stdout_is_tty()) GTEST_SKIP() << "stdout is a terminal";

    tc_term_options opt;
    tc_init_term_options(&opt);
    opt.headless = false;

    tc_term_t* term = reinterpret_cast<tc_term_t*>(1);   /* must be cleared */
    EXPECT_EQ(tc_term_create(&opt, &term), TC_ERR_NOT_A_TTY);
    EXPECT_EQ(term, nullptr);
}

TEST(TermLifecycle, EnterLeaveAndIdempotentLeave) {
    tc_term_options opt = headless_options();
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);

    bool active = false;
    EXPECT_EQ(tc_term_enter(term), TC_OK);
    ASSERT_EQ(tc_term_is_active(term, &active), TC_OK);
    EXPECT_TRUE(active);

    EXPECT_EQ(tc_term_enter(term), TC_ERR_STATE);   /* already active */

    EXPECT_EQ(tc_term_leave(term), TC_OK);
    ASSERT_EQ(tc_term_is_active(term, &active), TC_OK);
    EXPECT_FALSE(active);

    EXPECT_EQ(tc_term_leave(term), TC_OK);   /* idempotent */

    tc_term_destroy(term);

    /* Leave before enter is a no-op, not an error. */
    tc_term_options opt2 = headless_options();
    tc_term_t*      term2 = nullptr;
    ASSERT_EQ(tc_term_create(&opt2, &term2), TC_OK);
    EXPECT_EQ(tc_term_leave(term2), TC_OK);
    tc_term_destroy(term2);
}

TEST(TermLifecycle, DestroyLeavesTheSessionFirst) {
    tc_alloc_counter counter;
    tc_alloc_counter_init(&counter, nullptr);

    tc_term_options opt = headless_options(&counter.allocator);
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);
    ASSERT_EQ(tc_term_enter(term), TC_OK);

    /* The handle is gone afterwards, so "restore on the way out" is observed
     * through the allocator: everything is released exactly once. */
    tc_term_destroy(term);
    EXPECT_TRUE(tc_alloc_counter_balanced(&counter));
}

TEST(TermLifecycle, RejectsBadArguments) {
    tc_term_options opt = headless_options();
    tc_term_t*      term = nullptr;

    EXPECT_EQ(tc_term_create(&opt, nullptr), TC_ERR_INVALID_ARG);
    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);

    EXPECT_EQ(tc_term_enter(nullptr), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_leave(nullptr), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_is_active(nullptr, nullptr), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_is_active(term, nullptr), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_get_size(nullptr, nullptr, nullptr), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_size(nullptr, 80, 24), TC_ERR_INVALID_ARG);

    tc_term_destroy(nullptr);   /* no-op */
    tc_term_destroy(term);
}

TEST(TermSize, HeadlessDefaultsAndOverride) {
    tc_term_options opt = headless_options();
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);

    int32_t cols = 0;
    int32_t rows = 0;
    ASSERT_EQ(tc_term_get_size(term, &cols, &rows), TC_OK);
    EXPECT_EQ(cols, 80);
    EXPECT_EQ(rows, 24);

    EXPECT_EQ(tc_term_set_size(term, 200, 50), TC_OK);
    ASSERT_EQ(tc_term_get_size(term, &cols, &rows), TC_OK);
    EXPECT_EQ(cols, 200);
    EXPECT_EQ(rows, 50);

    /* Individual out parameters are optional. */
    cols = -1;
    EXPECT_EQ(tc_term_get_size(term, &cols, nullptr), TC_OK);
    EXPECT_EQ(cols, 200);

    EXPECT_EQ(tc_term_set_size(term, 0, 10), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_size(term, 10, -1), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_size(term, 99999, 10), TC_ERR_INVALID_ARG);

    tc_term_destroy(term);
}

TEST(TermSize, PixelSizeIsZeroWhenUnavailable) {
    tc_term_options opt = headless_options();
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);

    int32_t w = -1;
    int32_t h = -1;
    EXPECT_EQ(tc_term_get_size_px(term, &w, &h), TC_ERR_UNSUPPORTED);
    EXPECT_EQ(w, 0);
    EXPECT_EQ(h, 0);

    tc_term_destroy(term);
}

TEST(TermMemory, SessionIsFullyReleased) {
    tc_alloc_counter counter;
    tc_alloc_counter_init(&counter, nullptr);

    tc_term_options opt = headless_options(&counter.allocator);
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);
    EXPECT_GT(counter.alloc_count, 0u);

    ASSERT_EQ(tc_term_enter(term), TC_OK);
    ASSERT_EQ(tc_term_leave(term), TC_OK);
    tc_term_destroy(term);

    EXPECT_TRUE(tc_alloc_counter_balanced(&counter));
    EXPECT_EQ(tc_alloc_counter_live_bytes(&counter), 0u);
}

TEST(TermMemory, DestroyWithoutLeaveIsStillBalanced) {
    tc_alloc_counter counter;
    tc_alloc_counter_init(&counter, nullptr);

    tc_term_options opt = headless_options(&counter.allocator);
    tc_term_t*      term = nullptr;

    ASSERT_EQ(tc_term_create(&opt, &term), TC_OK);
    ASSERT_EQ(tc_term_enter(term), TC_OK);
    tc_term_destroy(term);

    EXPECT_TRUE(tc_alloc_counter_balanced(&counter));
}
