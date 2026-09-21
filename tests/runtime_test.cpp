/* Runtime feature toggles and per-session attributes (docs/02 §4–§7, §10,
 * §12).
 *
 * White-box: internal state (requested / applied / cursor flags) is inspected
 * through term_internal.h. Every case runs headless (null backend), so the
 * sequences are no-ops and the suite stays deterministic under ctest.
 */
#include <gtest/gtest.h>

#include <termcore/tc.h>

#include <control/term_internal.h>

#include <cstdlib>
#include <string>

namespace {

/* Sets / restores an environment variable for the lifetime of the object. */
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name);
        had_old_        = (old != nullptr);
        if (had_old_) old_ = old;
        set(name, value);
    }
    ~ScopedEnv() {
        if (had_old_) {
            set(name_.c_str(), old_.c_str());
        } else {
            unset(name_.c_str());
        }
    }

private:
    static void set(const char* n, const char* v) {
#ifdef _WIN32
        _putenv_s(n, v);
#else
        setenv(n, v, 1);
#endif
    }
    static void unset(const char* n) {
#ifdef _WIN32
        _putenv_s(n, "");
#else
        unsetenv(n);
#endif
    }

    std::string name_;
    std::string old_;
    bool        had_old_ = false;
};

tc_term_options headless_options(const tc_allocator* alloc = nullptr) {
    tc_term_options opt;
    tc_init_term_options(&opt);
    opt.headless  = true;
    opt.allocator = alloc;
    return opt;
}

/* A default-options session: raw + altscreen + paste + kitty + sync on,
 * mouse off. */
tc_term_t* make_term(const tc_allocator* alloc = nullptr) {
    tc_term_options opt = headless_options(alloc);
    tc_term_t*      t   = nullptr;
    EXPECT_EQ(tc_term_create(&opt, &t), TC_OK);
    return t;
}

}  // namespace

/* --------------------------------------------------------------------------
 * set_feature in CREATED state: records the request, enter() applies it.
 * ------------------------------------------------------------------------ */

TEST(RuntimeFeature, CreatedStateOnlyRecordsTheRequest) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    /* Default: focus events off. */
    ASSERT_FALSE(m->requested[TC_FEATURE_FOCUS_EVENTS]);

    EXPECT_EQ(tc_term_set_feature(t, TC_FEATURE_FOCUS_EVENTS, true), TC_OK);
    EXPECT_TRUE(m->requested[TC_FEATURE_FOCUS_EVENTS]);
    /* Not active: nothing applied yet. */
    EXPECT_FALSE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    tc_term_destroy(t);
}

TEST(RuntimeFeature, CreatedStateToggleIsAppliedByEnter) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    /* Default: bracketed paste on. Turn it off before enter. */
    ASSERT_TRUE(m->requested[TC_FEATURE_BRACKETED_PASTE]);
    EXPECT_EQ(tc_term_set_feature(t, TC_FEATURE_BRACKETED_PASTE, false), TC_OK);
    ASSERT_FALSE(m->requested[TC_FEATURE_BRACKETED_PASTE]);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_FALSE(m->applied[TC_FEATURE_BRACKETED_PASTE]);

    tc_term_destroy(t);
}

TEST(RuntimeFeature, SettingTheCurrentValueIsIdempotent) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    /* Default: raw mode on. Setting it again is a no-op. */
    ASSERT_TRUE(m->requested[TC_FEATURE_RAW_MODE]);
    EXPECT_EQ(tc_term_set_feature(t, TC_FEATURE_RAW_MODE, true), TC_OK);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_RAW_MODE]);
    EXPECT_EQ(tc_term_set_feature(t, TC_FEATURE_RAW_MODE, true), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_RAW_MODE]);

    tc_term_destroy(t);
}

TEST(RuntimeFeature, ActiveToggleTakesEffectImmediately) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    ASSERT_FALSE(m->applied[TC_FEATURE_FOCUS_EVENTS]);   /* default: off */

    EXPECT_EQ(tc_term_set_feature(t, TC_FEATURE_FOCUS_EVENTS, true), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    EXPECT_EQ(tc_term_set_feature(t, TC_FEATURE_FOCUS_EVENTS, false), TC_OK);
    EXPECT_FALSE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    tc_term_destroy(t);
}

TEST(RuntimeFeature, RequestedStateSurvivesLeave) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    ASSERT_EQ(tc_term_set_feature(t, TC_FEATURE_FOCUS_EVENTS, true), TC_OK);
    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    /* leave() clears applied but keeps requested; a second enter re-applies. */
    ASSERT_EQ(tc_term_leave(t), TC_OK);
    EXPECT_FALSE(m->applied[TC_FEATURE_FOCUS_EVENTS]);
    EXPECT_TRUE(m->requested[TC_FEATURE_FOCUS_EVENTS]);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    tc_term_destroy(t);
}

TEST(RuntimeFeature, GetFeatureReportsRequestedAndEffective) {
    /* TERMCORE_CAPS = FOCUS_EVENTS (1 << 20): a deterministic conclusion no
     * matter what the ambient shell exports. */
    ScopedEnv caps_env("TERMCORE_CAPS", "0x100000");
    tc_term_t* t = make_term();

    bool req = false;
    bool eff = true;
    ASSERT_EQ(tc_term_set_feature(t, TC_FEATURE_FOCUS_EVENTS, true), TC_OK);
    ASSERT_EQ(tc_term_get_feature(t, TC_FEATURE_FOCUS_EVENTS, &req, &eff), TC_OK);
    EXPECT_TRUE(req);
    /* effective = requested && caps allow (docs/02 §4, docs/03 §10.2). */
    EXPECT_TRUE(eff);

    /* A request the caps reject stays requested but is not effective. */
    ASSERT_EQ(tc_term_set_feature(t, TC_FEATURE_KITTY_KEYBOARD, true), TC_OK);
    ASSERT_EQ(tc_term_get_feature(t, TC_FEATURE_KITTY_KEYBOARD, &req, &eff), TC_OK);
    EXPECT_TRUE(req);
    EXPECT_FALSE(eff);

    /* Either out pointer may be NULL. */
    EXPECT_EQ(tc_term_get_feature(t, TC_FEATURE_FOCUS_EVENTS, &req, nullptr), TC_OK);
    EXPECT_TRUE(req);
    EXPECT_EQ(tc_term_get_feature(t, TC_FEATURE_FOCUS_EVENTS, nullptr, nullptr), TC_OK);

    tc_term_destroy(t);
}

TEST(RuntimeFeature, RejectsBadFeatureArgument) {
    tc_term_t* t = make_term();

    EXPECT_EQ(tc_term_set_feature(t, (tc_feature)-1, true), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_feature(t, (tc_feature)TC_FEATURE_COUNT, true), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_get_feature(t, (tc_feature)TC_FEATURE_COUNT, nullptr, nullptr),
              TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_feature(nullptr, TC_FEATURE_RAW_MODE, true), TC_ERR_INVALID_ARG);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Mouse mode (docs/02 §4.1): four-valued, drives TC_FEATURE_MOUSE.
 * ------------------------------------------------------------------------ */

TEST(RuntimeMouseMode, OffToDragTogglesTheFeatureOn) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    ASSERT_EQ(tc_term_get_mouse_mode(t, nullptr), TC_ERR_INVALID_ARG);

    tc_mouse_mode mode = TC_MOUSE_MOTION;
    ASSERT_EQ(tc_term_get_mouse_mode(t, &mode), TC_OK);
    EXPECT_EQ(mode, TC_MOUSE_OFF);
    ASSERT_FALSE(m->requested[TC_FEATURE_MOUSE]);

    EXPECT_EQ(tc_term_set_mouse_mode(t, TC_MOUSE_DRAG), TC_OK);
    EXPECT_EQ(m->mouse_mode, TC_MOUSE_DRAG);
    EXPECT_TRUE(m->requested[TC_FEATURE_MOUSE]);

    /* Idempotent. */
    EXPECT_EQ(tc_term_set_mouse_mode(t, TC_MOUSE_DRAG), TC_OK);

    tc_term_destroy(t);
}

TEST(RuntimeMouseMode, ActiveSwitchWritesStateConsistently) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    ASSERT_EQ(tc_term_set_mouse_mode(t, TC_MOUSE_DRAG), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_MOUSE]);
    EXPECT_EQ(m->applied_mouse_mode, TC_MOUSE_DRAG);

    /* Switching modes undoes the old one first. */
    EXPECT_EQ(tc_term_set_mouse_mode(t, TC_MOUSE_CLICK), TC_OK);
    EXPECT_EQ(m->applied_mouse_mode, TC_MOUSE_CLICK);
    EXPECT_TRUE(m->applied[TC_FEATURE_MOUSE]);

    /* Back to OFF clears the feature. */
    EXPECT_EQ(tc_term_set_mouse_mode(t, TC_MOUSE_OFF), TC_OK);
    EXPECT_FALSE(m->applied[TC_FEATURE_MOUSE]);
    EXPECT_FALSE(m->requested[TC_FEATURE_MOUSE]);

    tc_term_destroy(t);
}

TEST(RuntimeMouseMode, RejectsOutOfRangeMode) {
    tc_term_t* t = make_term();
    EXPECT_EQ(tc_term_set_mouse_mode(t, (tc_mouse_mode)99), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_mouse_mode(nullptr, TC_MOUSE_DRAG), TC_ERR_INVALID_ARG);
    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Raw / altscreen convenience entries (docs/02 §5, §6).
 * ------------------------------------------------------------------------ */

TEST(RuntimeRaw, ToggleSharesTheFeatureState) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    bool raw = false;
    ASSERT_EQ(tc_term_is_raw(t, &raw), TC_OK);
    EXPECT_TRUE(raw);   /* default: on */

    EXPECT_EQ(tc_term_set_raw(t, false), TC_OK);
    ASSERT_EQ(tc_term_is_raw(t, &raw), TC_OK);
    EXPECT_FALSE(raw);
    EXPECT_FALSE(m->requested[TC_FEATURE_RAW_MODE]);

    EXPECT_EQ(tc_term_set_raw(t, true), TC_OK);
    ASSERT_EQ(tc_term_is_raw(t, &raw), TC_OK);
    EXPECT_TRUE(raw);

    EXPECT_EQ(tc_term_set_raw(nullptr, true), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_is_raw(nullptr, &raw), TC_ERR_INVALID_ARG);

    tc_term_destroy(t);
}

TEST(RuntimeAltScreen, EnterLeaveShareTheFeatureState) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    /* Default: alternate screen on. */
    ASSERT_TRUE(m->requested[TC_FEATURE_ALT_SCREEN]);
    EXPECT_EQ(tc_term_leave_alt_screen(t), TC_OK);
    EXPECT_FALSE(m->requested[TC_FEATURE_ALT_SCREEN]);
    EXPECT_EQ(tc_term_enter_alt_screen(t), TC_OK);
    EXPECT_TRUE(m->requested[TC_FEATURE_ALT_SCREEN]);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_ALT_SCREEN]);
    EXPECT_EQ(tc_term_leave_alt_screen(t), TC_OK);
    EXPECT_FALSE(m->applied[TC_FEATURE_ALT_SCREEN]);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Cursor (docs/02 §7)
 * ------------------------------------------------------------------------ */

TEST(RuntimeCursor, VisibilityIsIdempotentAndSurvivesEnter) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    /* Default: hidden (hide_cursor = true). */
    ASSERT_TRUE(m->hide_cursor);
    EXPECT_EQ(tc_term_set_cursor_visible(t, true), TC_OK);
    EXPECT_FALSE(m->hide_cursor);

    /* Same value again: no-op. */
    EXPECT_EQ(tc_term_set_cursor_visible(t, true), TC_OK);

    EXPECT_EQ(tc_term_set_cursor_visible(t, false), TC_OK);
    EXPECT_TRUE(m->hide_cursor);
    EXPECT_FALSE(m->cursor_hidden);   /* not applied yet */

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_TRUE(m->cursor_hidden);

    tc_term_destroy(t);
}

TEST(RuntimeCursor, ShapeIsRecordedBeforeEnterAndAppliedByEnter) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    ASSERT_EQ(m->cursor_shape, TC_CURSOR_DEFAULT);
    EXPECT_EQ(tc_term_set_cursor_shape(t, TC_CURSOR_BAR), TC_OK);
    EXPECT_EQ(m->cursor_shape, TC_CURSOR_BAR);
    EXPECT_FALSE(m->cursor_shape_applied);   /* not active yet */

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_TRUE(m->cursor_shape_applied);

    /* Idempotent on the active session. */
    EXPECT_EQ(tc_term_set_cursor_shape(t, TC_CURSOR_BAR), TC_OK);

    /* Active change takes effect immediately. */
    EXPECT_EQ(tc_term_set_cursor_shape(t, TC_CURSOR_BLINK_BLOCK), TC_OK);
    EXPECT_EQ(m->cursor_shape, TC_CURSOR_BLINK_BLOCK);

    /* Back to default: no restore needed on leave. */
    EXPECT_EQ(tc_term_set_cursor_shape(t, TC_CURSOR_DEFAULT), TC_OK);
    EXPECT_FALSE(m->cursor_shape_applied);

    tc_term_destroy(t);
}

TEST(RuntimeCursor, LeaveRestoresTheChangedShape) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_EQ(tc_term_set_cursor_shape(t, TC_CURSOR_UNDERLINE), TC_OK);
    EXPECT_TRUE(m->cursor_shape_applied);

    ASSERT_EQ(tc_term_leave(t), TC_OK);
    EXPECT_FALSE(m->cursor_shape_applied);
    /* Requested shape survives for a re-enter. */
    EXPECT_EQ(m->cursor_shape, TC_CURSOR_UNDERLINE);

    tc_term_destroy(t);
}

TEST(RuntimeCursor, PositionIsRejectedWhenNegative) {
    tc_term_t* t = make_term();

    EXPECT_EQ(tc_term_set_cursor_pos(t, -1, 0), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_cursor_pos(t, 0, -1), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_cursor_pos(t, 0, 0), TC_OK);   /* CREATED: transient, no-op */
    EXPECT_EQ(tc_term_set_cursor_pos(nullptr, 0, 0), TC_ERR_INVALID_ARG);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_EQ(tc_term_set_cursor_pos(t, 3, 5), TC_OK);   /* ACTIVE: goes to backend */

    tc_term_destroy(t);
}

TEST(RuntimeCursor, ShapeRejectsOutOfRange) {
    tc_term_t* t = make_term();
    EXPECT_EQ(tc_term_set_cursor_shape(t, (tc_cursor_shape)99), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_cursor_shape(nullptr, TC_CURSOR_BAR), TC_ERR_INVALID_ARG);
    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Title (docs/02 §10)
 * ------------------------------------------------------------------------ */

TEST(RuntimeTitle, NullBackendAcceptsAndNullStringIsRejected) {
    tc_term_t* t = make_term();

    EXPECT_EQ(tc_term_set_title(t, ""), TC_OK);
    EXPECT_EQ(tc_term_set_title(t, "hello \xe4\xb8\xad\xe6\x96\x87"), TC_OK);
    EXPECT_EQ(tc_term_set_title(nullptr, "x"), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_title(t, nullptr), TC_ERR_INVALID_ARG);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Text mode (docs/02 §12)
 * ------------------------------------------------------------------------ */

TEST(RuntimeTextMode, ModeAndAmbiguousWideAreRuntimeMutable) {
    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);

    tc_text_mode mode = TC_TEXT_UNICODE;
    ASSERT_EQ(tc_term_get_text_mode(t, &mode), TC_OK);
    EXPECT_EQ(mode, TC_TEXT_UNICODE);   /* default */

    EXPECT_EQ(tc_term_set_text_mode(t, TC_TEXT_ASCII), TC_OK);
    ASSERT_EQ(tc_term_get_text_mode(t, &mode), TC_OK);
    EXPECT_EQ(mode, TC_TEXT_ASCII);

    /* A surface created afterwards inherits the current value. */
    tc_surface_t* s = nullptr;
    ASSERT_EQ(tc_surface_create(t, 10, 5, &s), TC_OK);

    EXPECT_EQ(tc_term_set_ambiguous_wide(t, true), TC_OK);
    EXPECT_TRUE(m->ambiguous_wide);

    EXPECT_EQ(tc_term_set_text_mode(t, (tc_text_mode)99), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_set_text_mode(nullptr, TC_TEXT_ASCII), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_get_text_mode(nullptr, &mode), TC_ERR_INVALID_ARG);

    tc_surface_destroy(s);
    tc_term_destroy(t);
}
