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
#include <control/term_internal.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

/* --------------------------------------------------------------------------
 * Override (docs/03 §9): the whole conclusion is replaced, NULL clears back
 * to the detection chain.
 * ------------------------------------------------------------------------ */

TEST(CapsOverride, ReplacesTheEffectiveConclusion) {
    NeutralCapsEnv neutral;   /* conservative default: 16 colours, no focus */

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps c;
    ASSERT_EQ(tc_term_get_caps(t, &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_16);

    tc_caps modern;
    memset(&modern, 0, sizeof(modern));
    modern.flags = TC_CAP_COLOR_16 | TC_CAP_COLOR_256 | TC_CAP_TRUECOLOR
        | TC_CAP_FOCUS_EVENTS;
    tc_caps c2;
    memset(&c2, 0, sizeof(c2));
    c2.color = TC_COLOR_LEVEL_TRUECOLOR;
    ASSERT_EQ(tc_term_set_caps_override(t, &c2), TC_OK);

    ASSERT_EQ(tc_term_get_caps(t, &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_TRUECOLOR);
    EXPECT_EQ(c.max_colors, 0);
    EXPECT_FALSE(tc_caps_has(&c, TC_CAP_FOCUS_EVENTS));   /* override replaces all */

    /* NULL clears: back to the conservative default. */
    ASSERT_EQ(tc_term_set_caps_override(t, nullptr), TC_OK);
    ASSERT_EQ(tc_term_get_caps(t, &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_16);
    EXPECT_EQ(c.mouse, TC_MOUSE_CLICK);

    tc_term_destroy(t);
}

TEST(CapsOverride, NullRestoresBaseExactly) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");   /* base = builtin 256 */

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps mono;
    memset(&mono, 0, sizeof(mono));
    mono.color = TC_COLOR_LEVEL_MONO;
    ASSERT_EQ(tc_term_set_caps_override(t, &mono), TC_OK);

    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);
    const tc_caps_entry* e = nullptr;
    ASSERT_EQ(tc_caps_get_entry(&p, TC_CAP_TRUECOLOR, &e), TC_OK);
    EXPECT_EQ(e->origin, TC_CAPS_ORIGIN_OVERRIDE);
    tc_caps_profile_dispose(&p);

    ASSERT_EQ(tc_term_set_caps_override(t, nullptr), TC_OK);

    /* The builtin provenance comes back with the base conclusion. */
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);
    ASSERT_EQ(tc_caps_get_entry(&p, TC_CAP_TRUECOLOR, &e), TC_OK);
    EXPECT_FALSE(e->supported);
    EXPECT_EQ(e->origin, TC_CAPS_ORIGIN_BUILTIN);
    tc_caps_profile_dispose(&p);

    tc_caps c;
    ASSERT_EQ(tc_term_get_caps(t, &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_256);

    tc_term_destroy(t);
}

TEST(CapsOverride, NullArgumentsAreRejected) {
    EXPECT_EQ(tc_term_set_caps_override(nullptr, nullptr), TC_ERR_INVALID_ARG);
}

/* --------------------------------------------------------------------------
 * Incremental bit writes (docs/03 §10.2)
 * ------------------------------------------------------------------------ */

TEST(CapsSetBits, TouchesOnlyTheGivenBits) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps before;
    ASSERT_EQ(tc_term_get_caps(t, &before), TC_OK);

    ASSERT_EQ(tc_term_set_caps_bits(t, 0, TC_CAP_TRUECOLOR, TC_CAPS_ORIGIN_INFER),
              TC_OK);

    tc_caps c;
    ASSERT_EQ(tc_term_get_caps(t, &c), TC_OK);
    EXPECT_EQ(c.flags, before.flags | TC_CAP_TRUECOLOR);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_TRUECOLOR);
    EXPECT_EQ(c.max_colors, 0);

    /* Untouched bits keep their builtin provenance. */
    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);
    const tc_caps_entry* e = nullptr;
    ASSERT_EQ(tc_caps_get_entry(&p, TC_CAP_TRUECOLOR, &e), TC_OK);
    EXPECT_TRUE(e->supported);
    EXPECT_EQ(e->origin, TC_CAPS_ORIGIN_INFER);
    ASSERT_EQ(tc_caps_get_entry(&p, TC_CAP_FOCUS_EVENTS, &e), TC_OK);
    EXPECT_EQ(e->origin, TC_CAPS_ORIGIN_BUILTIN);
    tc_caps_profile_dispose(&p);

    /* Clearing the bit drops the derived colour again. */
    ASSERT_EQ(tc_term_set_caps_bits(t, TC_CAP_TRUECOLOR, 0, TC_CAPS_ORIGIN_OVERRIDE),
              TC_OK);
    ASSERT_EQ(tc_term_get_caps(t, &c), TC_OK);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_256);

    tc_term_destroy(t);
}

TEST(CapsSetBits, ProbeWritesAreUserConfirmed) {
    NeutralCapsEnv neutral;

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    ASSERT_EQ(tc_term_set_caps_bits(t, 0, TC_CAP_FOCUS_EVENTS,
                                    TC_CAPS_ORIGIN_PROBE), TC_OK);

    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);
    const tc_caps_entry* e = nullptr;
    ASSERT_EQ(tc_caps_get_entry(&p, TC_CAP_FOCUS_EVENTS, &e), TC_OK);
    EXPECT_TRUE(e->supported);
    EXPECT_TRUE(e->user_confirmed);
    EXPECT_EQ(e->origin, TC_CAPS_ORIGIN_PROBE);
    tc_caps_profile_dispose(&p);

    /* A later non-probe write clears the confirmation. */
    ASSERT_EQ(tc_term_set_caps_bits(t, 0, TC_CAP_FOCUS_EVENTS,
                                    TC_CAPS_ORIGIN_OVERRIDE), TC_OK);
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);
    ASSERT_EQ(tc_caps_get_entry(&p, TC_CAP_FOCUS_EVENTS, &e), TC_OK);
    EXPECT_FALSE(e->user_confirmed);
    tc_caps_profile_dispose(&p);

    tc_term_destroy(t);
}

TEST(CapsSetBits, InvalidOriginIsRejected) {
    NeutralCapsEnv neutral;
    tc_term_t*     t = make_term();

    EXPECT_EQ(tc_term_set_caps_bits(t, 0, TC_CAP_FOCUS_EVENTS,
                                    (tc_caps_origin)99), TC_ERR_INVALID_ARG);

    tc_term_destroy(t);
}

TEST(CapsApplyProfile, FlagsAreAuthoritative) {
    NeutralCapsEnv neutral;

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    /* A profile that only claims focus + paste; everything else falls back to
     * unsupported (宁可低估, docs/03 §10.3). */
    tc_caps_profile p;
    memset(&p, 0, sizeof(p));
    p.flags = TC_CAP_FOCUS_EVENTS | TC_CAP_BRACKETED_PASTE;

    ASSERT_EQ(tc_term_apply_caps_profile(t, &p), TC_OK);

    tc_caps c;
    ASSERT_EQ(tc_term_get_caps(t, &c), TC_OK);
    EXPECT_EQ(c.flags, TC_CAP_FOCUS_EVENTS | TC_CAP_BRACKETED_PASTE);
    EXPECT_EQ(c.color, TC_COLOR_LEVEL_MONO);   /* no colour bits */
    EXPECT_EQ(c.mouse, TC_MOUSE_OFF);

    /* Entry provenance is applied per bit. */
    tc_caps_entry e;
    memset(&e, 0, sizeof(e));
    e.bit            = TC_CAP_FOCUS_EVENTS;
    e.supported      = true;
    e.origin         = TC_CAPS_ORIGIN_PROBE;
    e.user_confirmed = true;
    e.decided_at_ms  = 42;

    tc_caps_profile p2;
    memset(&p2, 0, sizeof(p2));
    p2.flags = TC_CAP_FOCUS_EVENTS;
    p2.entries = &e;
    p2.entry_count = 1;
    ASSERT_EQ(tc_term_apply_caps_profile(t, &p2), TC_OK);

    tc_caps_profile got;
    ASSERT_EQ(tc_term_get_caps_profile(t, &got), TC_OK);
    const tc_caps_entry* ge = nullptr;
    ASSERT_EQ(tc_caps_get_entry(&got, TC_CAP_FOCUS_EVENTS, &ge), TC_OK);
    EXPECT_TRUE(ge->user_confirmed);
    EXPECT_EQ(ge->origin, TC_CAPS_ORIGIN_PROBE);
    EXPECT_EQ(ge->decided_at_ms, 42);
    tc_caps_profile_dispose(&got);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Profile snapshot (docs/03 §10.1, §10.2)
 * ------------------------------------------------------------------------ */

TEST(CapsProfile, SnapshotCoversEveryDefinedBit) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);
    EXPECT_EQ(p.entry_count, 26u);
    EXPECT_EQ(p.flags, p.caps.flags);
    ASSERT_NE(p.entries, nullptr);
    /* entries were allocated with the term's allocator (default here). */
    EXPECT_NE(p.alloc, nullptr);

    /* Every defined bit has an entry; supported mirrors the bitset. */
    uint64_t seen = 0;
    for (size_t i = 0; i < p.entry_count; i++) {
        const tc_caps_entry* e = &p.entries[i];
        EXPECT_EQ(e->bit, 1ull << i);
        seen |= e->bit;
        EXPECT_EQ(e->supported, (p.flags & e->bit) != 0);
        EXPECT_EQ(e->origin, TC_CAPS_ORIGIN_BUILTIN);   /* builtin conclusion */
    }
    EXPECT_EQ(seen, TC_CAPS_BIT_MASK);

    tc_caps_profile_dispose(&p);
    tc_term_destroy(t);
}

TEST(CapsProfile, GetEntryFindsAndMisses) {
    NeutralCapsEnv neutral;

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);

    const tc_caps_entry* e = nullptr;
    ASSERT_EQ(tc_caps_get_entry(&p, TC_CAP_COLOR_16, &e), TC_OK);
    EXPECT_TRUE(e->supported);   /* conservative default has 16 colours */
    EXPECT_EQ(e->origin, TC_CAPS_ORIGIN_DEFAULT);

    EXPECT_EQ(tc_caps_get_entry(&p, 1ull << 40, &e), TC_ERR_NOT_FOUND);
    EXPECT_EQ(tc_caps_get_entry(nullptr, TC_CAP_COLOR_16, &e), TC_ERR_INVALID_ARG);

    tc_caps_profile_dispose(&p);
    tc_term_destroy(t);
}

TEST(CapsProfile, DisposedTwiceIsSafe) {
    NeutralCapsEnv neutral;

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);
    tc_caps_profile_dispose(&p);
    tc_caps_profile_dispose(&p);   /* double dispose must not crash */

    tc_term_destroy(t);
}

TEST(CapsProfile, EnvInferenceIsMarkedInfer) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");
    ScopedEnv      ct("COLORTERM", "truecolor");

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    /* The base builtin said 256; COLORTERM bumped the colour bits to
     * truecolor — those bits carry INFER provenance. */
    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);
    const tc_caps_entry* e = nullptr;
    ASSERT_EQ(tc_caps_get_entry(&p, TC_CAP_TRUECOLOR, &e), TC_OK);
    EXPECT_TRUE(e->supported);
    EXPECT_EQ(e->origin, TC_CAPS_ORIGIN_INFER);
    ASSERT_EQ(tc_caps_get_entry(&p, TC_CAP_FOCUS_EVENTS, &e), TC_OK);
    EXPECT_EQ(e->origin, TC_CAPS_ORIGIN_BUILTIN);
    tc_caps_profile_dispose(&p);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Fingerprint and matching (docs/03 §10.1, §10.3)
 * ------------------------------------------------------------------------ */

TEST(CapsFingerprint, CapturesTheEnvironment) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");
    ScopedEnv      prog("TERM_PROGRAM", "vscode");
    ScopedEnv      ct("COLORTERM", "truecolor");

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps_fingerprint fp;
    ASSERT_EQ(tc_term_get_caps_fingerprint(t, &fp), TC_OK);
    EXPECT_STREQ(fp.term, "xterm-256color");
    EXPECT_STREQ(fp.term_program, "vscode");
    EXPECT_STREQ(fp.colorterm, "truecolor");
    EXPECT_EQ(fp.da_primary_id, 0);   /* stage D fills */
    EXPECT_EQ(fp.da_version, 0);
    EXPECT_EQ(fp.lib_abi_version, TERMCORE_ABI_VERSION);
    EXPECT_EQ(fp.profile_version, TC_CAPS_PROFILE_VERSION);
    EXPECT_GT(fp.saved_at_ms, 0);

    EXPECT_EQ(tc_term_get_caps_fingerprint(nullptr, &fp), TC_ERR_INVALID_ARG);

    tc_term_destroy(t);
}

TEST(CapsMatch, SameFingerprintMatches) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);

    tc_caps_fingerprint now;
    ASSERT_EQ(tc_term_get_caps_fingerprint(t, &now), TC_OK);

    bool ok = false;
    ASSERT_EQ(tc_caps_profile_match(&p, &now, &ok), TC_OK);
    EXPECT_TRUE(ok);

    /* saved_at_ms is not part of the identity (docs/03 §10.3). */
    now.saved_at_ms += 123456;
    ASSERT_EQ(tc_caps_profile_match(&p, &now, &ok), TC_OK);
    EXPECT_TRUE(ok);

    /* A changed TERM means the environment may have changed. */
    strcpy(now.term, "vt100");
    ASSERT_EQ(tc_caps_profile_match(&p, &now, &ok), TC_OK);
    EXPECT_FALSE(ok);

    tc_caps_profile_dispose(&p);
    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Serialization round trips (docs/03 §10.3)
 * ------------------------------------------------------------------------ */

/* Builds a profile with interesting provenance, then round-trips it. */
TEST(CapsSerialize, BinaryRoundTrip) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(tc_term_set_caps_bits(t, 0, TC_CAP_FOCUS_EVENTS,
                                    TC_CAPS_ORIGIN_PROBE), TC_OK);

    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);

    size_t need = 0;
    ASSERT_EQ(tc_caps_profile_serialize(&p, nullptr, 0, &need), TC_OK);
    EXPECT_EQ(need, 32u + 26u * 32u + 196u);

    std::vector<uint8_t> blob(need);
    ASSERT_EQ(tc_caps_profile_serialize(&p, blob.data(), need, &need), TC_OK);

    tc_caps_profile back;
    ASSERT_EQ(tc_caps_profile_deserialize(blob.data(), need, &back), TC_OK);

    EXPECT_EQ(back.flags, p.flags);
    EXPECT_EQ(back.caps.color, p.caps.color);
    EXPECT_EQ(back.caps.max_colors, p.caps.max_colors);
    EXPECT_EQ(back.caps.mouse, p.caps.mouse);
    EXPECT_EQ(back.entry_count, p.entry_count);

    for (size_t i = 0; i < p.entry_count; i++) {
        const tc_caps_entry* a = &p.entries[i];
        const tc_caps_entry* b = &back.entries[i];
        EXPECT_EQ(b->bit, a->bit);
        EXPECT_EQ(b->supported, a->supported);
        EXPECT_EQ(b->origin, a->origin);
        EXPECT_EQ(b->user_confirmed, a->user_confirmed);
        EXPECT_EQ(b->decided_at_ms, a->decided_at_ms);
    }

    EXPECT_STREQ(back.fp.term, p.fp.term);
    EXPECT_STREQ(back.fp.term_program, p.fp.term_program);
    EXPECT_STREQ(back.fp.colorterm, p.fp.colorterm);
    EXPECT_EQ(back.fp.da_primary_id, p.fp.da_primary_id);
    EXPECT_EQ(back.fp.da_version, p.fp.da_version);
    EXPECT_EQ(back.fp.lib_abi_version, p.fp.lib_abi_version);
    EXPECT_EQ(back.fp.profile_version, p.fp.profile_version);

    tc_caps_profile_dispose(&back);
    tc_caps_profile_dispose(&p);
    tc_term_destroy(t);
}

TEST(CapsSerialize, BinaryRejectsBadInput) {
    NeutralCapsEnv neutral;
    tc_term_t*     t = make_term();
    ASSERT_NE(t, nullptr);

    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);
    size_t need = 0;
    ASSERT_EQ(tc_caps_profile_serialize(&p, nullptr, 0, &need), TC_OK);
    std::vector<uint8_t> blob(need);
    ASSERT_EQ(tc_caps_profile_serialize(&p, blob.data(), need, &need), TC_OK);

    /* Too small a target buffer. */
    size_t small = need - 1;
    EXPECT_EQ(tc_caps_profile_serialize(&p, blob.data(), small, &small),
              TC_ERR_OVERFLOW);

    tc_caps_profile_dispose(&p);

    /* Wrong magic. */
    tc_caps_profile out;
    std::vector<uint8_t> bad = blob;
    bad[0] = 'X';
    EXPECT_EQ(tc_caps_profile_deserialize(bad.data(), bad.size(), &out),
              TC_ERR_PARSE);

    /* Version mismatch. */
    bad = blob;
    bad[4] = 99;
    EXPECT_EQ(tc_caps_profile_deserialize(bad.data(), bad.size(), &out),
              TC_ERR_VERSION);

    /* Truncated buffer. */
    EXPECT_EQ(tc_caps_profile_deserialize(blob.data(), blob.size() - 10, &out),
              TC_ERR_PARSE);

    EXPECT_EQ(tc_caps_profile_deserialize(nullptr, 0, &out), TC_ERR_INVALID_ARG);

    tc_term_destroy(t);
}

TEST(CapsSerialize, TextRoundTrip) {
    NeutralCapsEnv neutral;
    ScopedEnv      term("TERM", "xterm-256color");

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(tc_term_set_caps_bits(t, 0, TC_CAP_FOCUS_EVENTS,
                                    TC_CAPS_ORIGIN_PROBE), TC_OK);

    tc_caps_profile p;
    ASSERT_EQ(tc_term_get_caps_profile(t, &p), TC_OK);

    size_t need = 0;
    ASSERT_EQ(tc_caps_profile_to_text(&p, nullptr, 0, &need), TC_OK);
    std::string text(need, '\0');
    ASSERT_EQ(tc_caps_profile_to_text(&p, text.data(), need, &need), TC_OK);
    text.resize(need - 1);

    tc_caps_profile back;
    ASSERT_EQ(tc_caps_profile_from_text(text.data(), text.size(), &back), TC_OK);

    EXPECT_EQ(back.flags, p.flags);
    EXPECT_EQ(back.caps.color, p.caps.color);
    EXPECT_EQ(back.caps.max_colors, p.caps.max_colors);
    EXPECT_EQ(back.caps.mouse, p.caps.mouse);
    EXPECT_EQ(back.entry_count, p.entry_count);

    for (size_t i = 0; i < p.entry_count; i++) {
        const tc_caps_entry* a = &p.entries[i];
        const tc_caps_entry* b = &back.entries[i];
        EXPECT_EQ(b->bit, a->bit);
        EXPECT_EQ(b->supported, a->supported);
        EXPECT_EQ(b->origin, a->origin);
        EXPECT_EQ(b->user_confirmed, a->user_confirmed);
        EXPECT_EQ(b->decided_at_ms, a->decided_at_ms);
    }

    EXPECT_STREQ(back.fp.term, p.fp.term);

    tc_caps_profile_dispose(&back);
    tc_caps_profile_dispose(&p);
    tc_term_destroy(t);
}

TEST(CapsSerialize, TextIsLenientAndStrict) {
    NeutralCapsEnv neutral;

    tc_term_t* t = make_term();
    ASSERT_NE(t, nullptr);

    /* Unknown keys are ignored; missing version / flags is a parse error. */
    tc_caps_profile out;
    const char* missing_required = "flags=0x100000\n";
    EXPECT_EQ(tc_caps_profile_from_text(missing_required,
                                        strlen(missing_required), &out),
              TC_ERR_PARSE);

    const char* version_mismatch = "version=999\nflags=0x100000\n";
    EXPECT_EQ(tc_caps_profile_from_text(version_mismatch,
                                        strlen(version_mismatch), &out),
              TC_ERR_VERSION);

    /* A malformed entry line fails the parse. */
    const char* bad_entry = "version=1\nflags=0x100000\nentry=0xZZ:1:probe:0:5\n";
    EXPECT_EQ(tc_caps_profile_from_text(bad_entry, strlen(bad_entry), &out),
              TC_ERR_PARSE);

    /* Minimal but valid text derives colour / mouse from the bitset. */
    const char* minimal = "version=1\nflags=0x4\nuser_key=ignored\n";
    ASSERT_EQ(tc_caps_profile_from_text(minimal, strlen(minimal), &out), TC_OK);
    EXPECT_EQ(out.flags, TC_CAP_TRUECOLOR);
    EXPECT_EQ(out.caps.color, TC_COLOR_LEVEL_TRUECOLOR);
    EXPECT_EQ(out.caps.mouse, TC_MOUSE_OFF);
    tc_caps_profile_dispose(&out);

    EXPECT_EQ(tc_caps_profile_from_text(nullptr, 0, &out), TC_ERR_INVALID_ARG);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Caps-gated replay on write (docs/03 §10.2)
 * ------------------------------------------------------------------------ */

TEST(CapsReplay, TurnsOffDisallowedFeatures) {
    NeutralCapsEnv neutral;   /* conservative caps: no focus events */

    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);
    ASSERT_NE(t, nullptr);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    ASSERT_EQ(tc_term_set_feature(t, TC_FEATURE_FOCUS_EVENTS, true), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    /* Re-state the (disallowing) conclusion: the feature must go off. */
    tc_caps c;
    tc_caps_conservative_default(&c);
    ASSERT_EQ(tc_term_set_caps_override(t, &c), TC_OK);
    EXPECT_FALSE(m->applied[TC_FEATURE_FOCUS_EVENTS]);
    EXPECT_TRUE(m->requested[TC_FEATURE_FOCUS_EVENTS]);   /* request survives */

    tc_term_destroy(t);
}

TEST(CapsReplay, ReAppliesWhenTheConclusionAllowsAgain) {
    NeutralCapsEnv neutral;
    ScopedEnv      caps_env("TERMCORE_CAPS", "0x100000");   /* focus only */

    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);
    ASSERT_NE(t, nullptr);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    ASSERT_EQ(tc_term_set_feature(t, TC_FEATURE_FOCUS_EVENTS, true), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    /* Take focus away... */
    tc_caps c;
    tc_caps_conservative_default(&c);
    ASSERT_EQ(tc_term_set_caps_override(t, &c), TC_OK);
    EXPECT_FALSE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    /* ...and the clear restores the base conclusion, which grants it. */
    ASSERT_EQ(tc_term_set_caps_override(t, nullptr), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    tc_term_destroy(t);
}

TEST(CapsReplay, MouseDegradesThroughTheChain) {
    NeutralCapsEnv neutral;

    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);
    ASSERT_NE(t, nullptr);

    ASSERT_EQ(tc_term_enter(t), TC_OK);
    ASSERT_EQ(tc_term_set_mouse_mode(t, TC_MOUSE_MOTION), TC_OK);
    EXPECT_EQ(m->applied_mouse_mode, TC_MOUSE_MOTION);

    /* A conclusion that only supports clicks degrades the applied mode. */
    tc_caps click;
    memset(&click, 0, sizeof(click));
    click.flags = TC_CAP_MOUSE_X10;
    click.mouse = TC_MOUSE_CLICK;
    ASSERT_EQ(tc_term_set_caps_override(t, &click), TC_OK);
    EXPECT_EQ(m->applied_mouse_mode, TC_MOUSE_CLICK);
    EXPECT_TRUE(m->applied[TC_FEATURE_MOUSE]);

    /* No mouse at all turns the feature off, request stays. */
    tc_caps no_mouse;
    memset(&no_mouse, 0, sizeof(no_mouse));
    ASSERT_EQ(tc_term_set_caps_override(t, &no_mouse), TC_OK);
    EXPECT_EQ(m->applied_mouse_mode, TC_MOUSE_OFF);
    EXPECT_FALSE(m->applied[TC_FEATURE_MOUSE]);
    EXPECT_TRUE(m->requested[TC_FEATURE_MOUSE]);

    /* Back to the base conclusion (X10 click): re-applied at click. */
    ASSERT_EQ(tc_term_set_caps_override(t, nullptr), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_MOUSE]);
    EXPECT_EQ(m->applied_mouse_mode, TC_MOUSE_CLICK);

    tc_term_destroy(t);
}

TEST(CapsReplay, NoopOutsideActiveState) {
    NeutralCapsEnv neutral;

    tc_term_t* t = make_term();
    tc_term*   m = reinterpret_cast<tc_term*>(t);
    ASSERT_NE(t, nullptr);

    /* CREATED: no applied state, replay is a no-op. */
    ASSERT_EQ(tc_term_set_feature(t, TC_FEATURE_FOCUS_EVENTS, true), TC_OK);
    tc_caps c;
    tc_caps_conservative_default(&c);
    ASSERT_EQ(tc_term_set_caps_override(t, &c), TC_OK);
    EXPECT_FALSE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    /* enter() still applies the request (the caps gate is a write-time
     * concern in this stage; enter gating lands with the full replay). */
    ASSERT_EQ(tc_term_enter(t), TC_OK);
    EXPECT_TRUE(m->applied[TC_FEATURE_FOCUS_EVENTS]);

    tc_term_destroy(t);
}
