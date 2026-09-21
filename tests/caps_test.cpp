/* Capability layer (docs/03): core model, detection chain and query API.
 *
 * Stage A white-box: the conservative default and the built-in TERM database
 * are internal (caps_internal.h), so they are tested directly; the environment
 * inference runs under controlled env vars (ScopedEnv) so the suite stays
 * deterministic no matter what the CI shell exports.
 */
#include <gtest/gtest.h>

#include <termcore/tc.h>

#include <caps/caps_internal.h>

#include <cstdlib>
#include <cstring>
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

/* Neutralises every env var the detection chain reads, so a test can set the
 * one it cares about in isolation. */
class NeutralCapsEnv {
public:
    NeutralCapsEnv()
        : term_("TERM", ""),
          prog_("TERM_PROGRAM", ""),
          ct_("COLORTERM", ""),
          nc_("NO_COLOR", ""),
          lvl_("TERMCORE_COLOR_LEVEL", ""),
          bits_("TERMCORE_CAPS", "") {}

private:
    ScopedEnv term_, prog_, ct_, nc_, lvl_, bits_;
};

tc_term_t* make_term() {
    tc_term_options opt;
    tc_init_term_options(&opt);
    opt.headless = true;
    tc_term_t*   t = nullptr;
    EXPECT_EQ(tc_term_create(&opt, &t), TC_OK);
    return t;
}

}  // namespace

/* --------------------------------------------------------------------------
 * Bitset query (docs/03 §1)
 * ------------------------------------------------------------------------ */

TEST(CapsHas, ReportsSingleBits) {
    tc_caps c;
    memset(&c, 0, sizeof(c));
    c.flags = TC_CAP_TRUECOLOR | TC_CAP_MOUSE_SGR | TC_CAP_UNICODE;

    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_TRUECOLOR));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_MOUSE_SGR));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_UNICODE));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_COLOR_256));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_STYLE_ITALIC));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_NONE));
}

TEST(CapsHas, NullCapsIsFalse) {
    EXPECT_FALSE(tc_caps_has(nullptr, TC_CAP_TRUECOLOR));
}

/* --------------------------------------------------------------------------
 * Conservative default (docs/03 §2 step 6)
 * ------------------------------------------------------------------------ */

TEST(CapsDefault, IsMinimalButSafe) {
    tc_caps c;
    tc_caps_conservative_default(&c);

    /* 16 colours, X10 click reporting. */
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_16);
    EXPECT_EQ(c.max_colors, 16);
    EXPECT_EQ(c.mouse, TC_MOUSE_CLICK);

    /* Safe primitives and SGR styles are present. */
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_COLOR_16));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_ALT_SCREEN));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_CURSOR_SHAPE));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_CURSOR_HIDE));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_TITLE));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_STYLE_BOLD));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_STYLE_ITALIC));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_STYLE_UNDERLINE));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_MOUSE_X10));

    /* Over-estimating these garbles output; the default must not claim them. */
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_COLOR_256));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_TRUECOLOR));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_UNICODE));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_PIXEL_SIZE));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_SYNC_UPDATE));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_FOCUS_EVENTS));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_BRACKETED_PASTE));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_KITTY_KEYBOARD));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_MODIFY_OTHER));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_MOUSE_DRAG));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_MOUSE_MOVE));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_MOUSE_SGR));
}

/* --------------------------------------------------------------------------
 * Built-in TERM database (docs/03 §5)
 * ------------------------------------------------------------------------ */

TEST(CapsBuiltin, Xterm256color) {
    tc_caps c;
    ASSERT_EQ(tc_caps_from_builtin("xterm-256color", &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_256);
    EXPECT_EQ(c.max_colors, 256);
    EXPECT_EQ(c.mouse, TC_MOUSE_DRAG);
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_MODIFY_OTHER));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_UNICODE));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_TRUECOLOR));
}

TEST(CapsBuiltin, DumbIsLowest) {
    tc_caps c;
    ASSERT_EQ(tc_caps_from_builtin("dumb", &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_MONO);
    EXPECT_EQ(c.max_colors, 0);
    EXPECT_EQ(c.mouse, TC_MOUSE_OFF);
    EXPECT_EQ(c.flags, TC_CAP_NONE);
}

TEST(CapsBuiltin, KittyHasKittyKeyboardAndSync) {
    tc_caps c;
    ASSERT_EQ(tc_caps_from_builtin("kitty", &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_TRUECOLOR);
    EXPECT_EQ(c.mouse, TC_MOUSE_MOTION);
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_KITTY_KEYBOARD));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_SYNC_UPDATE));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_FOCUS_EVENTS));
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_BRACKETED_PASTE));
}

TEST(CapsBuiltin, MsTerminalIsModern) {
    tc_caps c;
    ASSERT_EQ(tc_caps_from_builtin("ms-terminal", &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_TRUECOLOR);
    EXPECT_EQ(c.mouse, TC_MOUSE_MOTION);
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_UNICODE));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_KITTY_KEYBOARD));   /* no kitty protocol */
}

TEST(CapsBuiltin, UnknownTermIsUnsupported) {
    tc_caps c;
    EXPECT_EQ(tc_caps_from_builtin("no-such-term", &c), TC_ERR_UNSUPPORTED);
    EXPECT_EQ(tc_caps_from_builtin("", &c), TC_ERR_UNSUPPORTED);
    EXPECT_EQ(tc_caps_from_builtin(nullptr, &c), TC_ERR_UNSUPPORTED);
    EXPECT_EQ(tc_caps_from_builtin("xterm", nullptr), TC_ERR_INVALID_ARG);
}

/* --------------------------------------------------------------------------
 * Environment inference (docs/03 §2 step 2, §4)
 * ------------------------------------------------------------------------ */

TEST(CapsFromEnv, TermNameInfers256) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_256);
    EXPECT_EQ(c.max_colors, 256);
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_COLOR_256));
}

TEST(CapsFromEnv, TermNameInfersTruecolor) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "wezterm-direct");

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_TRUECOLOR);
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_TRUECOLOR));
}

TEST(CapsFromEnv, ColortermTruecolorWinsOverTerm) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");
    ScopedEnv      ct("COLORTERM", "truecolor");

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_TRUECOLOR);
}

TEST(CapsFromEnv, NoColorForcesMono) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");
    ScopedEnv      nc("NO_COLOR", "1");

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_MONO);
    EXPECT_EQ(c.max_colors, 0);
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_COLOR_16));
}

TEST(CapsFromEnv, NoColorZeroIsIgnored) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");
    ScopedEnv      nc("NO_COLOR", "0");

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_256);   /* not forced to mono */
}

TEST(CapsFromEnv, ColorLevelExplicitWinsOverNoColor) {
    NeutralCapsEnv neutral;
    ScopedEnv      nc("NO_COLOR", "1");
    ScopedEnv      lvl("TERMCORE_COLOR_LEVEL", "256");

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_256);   /* explicit config > NO_COLOR */
}

TEST(CapsFromEnv, ColorLevelAcceptsAllValues) {
    NeutralCapsEnv neutral;
    ScopedEnv      lvl("TERMCORE_COLOR_LEVEL", "mono");

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_MONO);
}

TEST(CapsFromEnv, UnknownEnvLeavesConservativeDefault) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "weird-term-name");

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_16);
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_ALT_SCREEN));
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_UNICODE));
}

TEST(CapsFromEnv, CapsBitsetOverridesEverything) {
    NeutralCapsEnv neutral;
    ScopedEnv      nc("NO_COLOR", "1");
    ScopedEnv      bits("TERMCORE_CAPS", "0x7");   /* 16 | 256 | truecolor */

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.flags, TC_CAP_COLOR_16 | TC_CAP_COLOR_256 | TC_CAP_TRUECOLOR);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_TRUECOLOR);
    EXPECT_EQ(c.max_colors, 0);
    EXPECT_EQ(c.mouse, TC_MOUSE_OFF);   /* no mouse bits present */
}

TEST(CapsFromEnv, CapsBitsetDerivesMouse) {
    NeutralCapsEnv neutral;
    ScopedEnv      bits("TERMCORE_CAPS", "0x60000");   /* MOUSE_DRAG | MOUSE_MOVE */

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.mouse, TC_MOUSE_MOTION);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_MONO);   /* no colour bits present */
}

TEST(CapsFromEnv, MalformedCapsBitsetIsIgnored) {
    NeutralCapsEnv neutral;
    ScopedEnv      nc("NO_COLOR", "1");
    ScopedEnv      bits("TERMCORE_CAPS", "not-a-number");

    tc_caps c;
    ASSERT_EQ(tc_caps_from_env(&c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_MONO);   /* NO_COLOR still applied */
}

TEST(CapsFromEnv, NullOutIsRejected) {
    EXPECT_EQ(tc_caps_from_env(nullptr), TC_ERR_INVALID_ARG);
}

/* --------------------------------------------------------------------------
 * Query API through a term (docs/03 §1)
 * ------------------------------------------------------------------------ */

TEST(CapsQuery, NullArgumentsAreRejected) {
    EXPECT_EQ(tc_term_get_caps(nullptr, nullptr), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_term_get_color_level(nullptr, nullptr), TC_ERR_INVALID_ARG);
}

TEST(CapsQuery, HeadlessTermReturnsEnvironmentConclusion) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps c;
    ASSERT_EQ(tc_term_get_caps(t, &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_256);
    EXPECT_TRUE(tc_caps_has(&c, TC_CAP_UNICODE));

    tc_color_level lvl = TC_COLOR_LEVEL_MONO;
    ASSERT_EQ(tc_term_get_color_level(t, &lvl), TC_OK);
    EXPECT_EQ(lvl, TC_COLOR_LEVEL_256);

    tc_term_destroy(t);
}

TEST(CapsQuery, HeadlessTermDefaultsWithoutEnv) {
    NeutralCapsEnv neutral;   /* TERM cleared */

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps c;
    ASSERT_EQ(tc_term_get_caps(t, &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_16);
    EXPECT_EQ(c.mouse, TC_MOUSE_CLICK);
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_UNICODE));

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Colour degradation, pure functions (docs/03 §3)
 * ------------------------------------------------------------------------ */

TEST(CapsDowngrade, TruecolorPassesThrough) {
    tc_caps c;
    memset(&c, 0, sizeof(c));
    c.color = TC_COLOR_LEVEL_TRUECOLOR;

    tc_color out;
    ASSERT_EQ(tc_caps_downgrade_rgb(&c, 255, 128, 0, &out), TC_OK);
    EXPECT_EQ(out.kind, TC_COLOR_RGB);
    EXPECT_EQ(out.r, 255);
    EXPECT_EQ(out.g, 128);
    EXPECT_EQ(out.b, 0);

    ASSERT_EQ(tc_caps_downgrade_indexed(&c, 42, &out), TC_OK);
    EXPECT_EQ(out.kind, TC_COLOR_INDEXED);
    EXPECT_EQ(out.idx, 42);
}

TEST(CapsDowngrade, Level256MapsRgbToNearestIndex) {
    tc_caps c;
    memset(&c, 0, sizeof(c));
    c.color = TC_COLOR_LEVEL_256;

    tc_color out;

    /* Pure red resolves to the system bright red (9), not the cube entry 196:
     * the scan starts at 0 and an exact tie keeps the lower index. */
    ASSERT_EQ(tc_caps_downgrade_rgb(&c, 255, 0, 0, &out), TC_OK);
    EXPECT_EQ(out.kind, TC_COLOR_INDEXED);
    EXPECT_EQ(out.idx, 9);

    /* Mid gray is in the system palette itself. */
    ASSERT_EQ(tc_caps_downgrade_rgb(&c, 128, 128, 128, &out), TC_OK);
    EXPECT_EQ(out.idx, 8);

    /* 100,100,100 -> gray ramp 241 = (98,98,98). */
    ASSERT_EQ(tc_caps_downgrade_rgb(&c, 100, 100, 100, &out), TC_OK);
    EXPECT_EQ(out.idx, 241);

    /* 255,128,0 -> cube 208 = (255,135,0). */
    ASSERT_EQ(tc_caps_downgrade_rgb(&c, 255, 128, 0, &out), TC_OK);
    EXPECT_EQ(out.idx, 208);
}

TEST(CapsDowngrade, Level256PassesIndexThrough) {
    tc_caps c;
    memset(&c, 0, sizeof(c));
    c.color = TC_COLOR_LEVEL_256;

    tc_color out;
    ASSERT_EQ(tc_caps_downgrade_indexed(&c, 42, &out), TC_OK);
    EXPECT_EQ(out.kind, TC_COLOR_INDEXED);
    EXPECT_EQ(out.idx, 42);
}

TEST(CapsDowngrade, Level16MapsToSystemPalette) {
    tc_caps c;
    memset(&c, 0, sizeof(c));
    c.color = TC_COLOR_LEVEL_16;

    tc_color out;

    ASSERT_EQ(tc_caps_downgrade_rgb(&c, 255, 0, 0, &out), TC_OK);
    EXPECT_EQ(out.kind, TC_COLOR_INDEXED);
    EXPECT_EQ(out.idx, 9);

    /* Index 42 = (0,215,135): nearest system colour is 6 = (0,128,128). */
    ASSERT_EQ(tc_caps_downgrade_indexed(&c, 42, &out), TC_OK);
    EXPECT_EQ(out.kind, TC_COLOR_INDEXED);
    EXPECT_EQ(out.idx, 6);
}

TEST(CapsDowngrade, MonoDropsColor) {
    tc_caps c;
    memset(&c, 0, sizeof(c));
    c.color = TC_COLOR_LEVEL_MONO;

    tc_color out;
    ASSERT_EQ(tc_caps_downgrade_rgb(&c, 255, 0, 0, &out), TC_OK);
    EXPECT_EQ(out.kind, TC_COLOR_DEFAULT);

    ASSERT_EQ(tc_caps_downgrade_indexed(&c, 42, &out), TC_OK);
    EXPECT_EQ(out.kind, TC_COLOR_DEFAULT);
}

TEST(CapsDowngrade, NullArgumentsAreRejected) {
    tc_caps c;
    memset(&c, 0, sizeof(c));

    EXPECT_EQ(tc_caps_downgrade_rgb(nullptr, 1, 2, 3, nullptr),
              TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_caps_downgrade_rgb(&c, 1, 2, 3, nullptr),
              TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_caps_downgrade_indexed(nullptr, 1, nullptr),
              TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_caps_downgrade_indexed(&c, 1, nullptr),
              TC_ERR_INVALID_ARG);
}
