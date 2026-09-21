#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <caps/caps_internal.h>

#include <control/term_internal.h>

#include <termcore/tc_version.h>

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* --------------------------------------------------------------------------
 * Capability layer (docs/03).
 *
 * Stage A scope: the core model (tc_caps bitset + derived color/mouse fields),
 * the detection chain (environment inference -> built-in TERM database ->
 * conservative default; DA query and terminfo arrive in later stages) and the
 * query API. Override / profile / serialization / probe land in later stages.
 *
 * "宁可低估" (docs/03 §2): an unknown step falls through to the next one and
 * the conservative default is deliberately minimal. SGR styles are safe to
 * emit even when unsupported (the terminal ignores them), so they are included
 * in the default; wide-char support (UNICODE) is not, because over-estimating
 * it garbles the layout (docs/03 §8).
 * ------------------------------------------------------------------------ */

/* --------------------------------------------------------------------------
 * Bitset query (docs/03 §1)
 * ------------------------------------------------------------------------ */

bool tc_caps_has(const tc_caps* c, tc_cap flag) {
    return c && (c->flags & (uint64_t)flag) != 0;
}

/* Monotonic millisecond clock for the per-bit provenance timestamps
 * (docs/03 §10.1). */
static int64_t caps_now_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* --------------------------------------------------------------------------
 * Control-layer gating (docs/03 §7, §9, §10.2)
 * ------------------------------------------------------------------------ */

bool tc_caps_allows_feature(const tc_caps* caps, tc_feature f) {
    if (!caps) return true;   /* term without caps state: never gate */

    switch (f) {
    case TC_FEATURE_ALT_SCREEN:
        return tc_caps_has(caps, TC_CAP_ALT_SCREEN);
    case TC_FEATURE_MOUSE:
        return caps->mouse != TC_MOUSE_OFF;
    case TC_FEATURE_FOCUS_EVENTS:
        return tc_caps_has(caps, TC_CAP_FOCUS_EVENTS);
    case TC_FEATURE_BRACKETED_PASTE:
        return tc_caps_has(caps, TC_CAP_BRACKETED_PASTE);
    case TC_FEATURE_KITTY_KEYBOARD:
        return tc_caps_has(caps, TC_CAP_KITTY_KEYBOARD);
    case TC_FEATURE_SYNC_UPDATE:
        return tc_caps_has(caps, TC_CAP_SYNC_UPDATE);
    default:
        /* RAW_MODE / CAPTURE_CTRL_C have no protocol bit of their own. */
        return true;
    }
}

/* The degradation chain is ordered OFF < CLICK < DRAG < MOTION (the enum
 * values), so the effective mode is simply the lower of the two. */
tc_mouse_mode tc_caps_effective_mouse(const tc_caps* caps, tc_mouse_mode requested) {
    tc_mouse_mode supported = caps ? caps->mouse : TC_MOUSE_OFF;

    if (supported == TC_MOUSE_OFF || requested == TC_MOUSE_OFF) return TC_MOUSE_OFF;
    return requested < supported ? requested : supported;
}

/* The color bits that a level implies (docs/03 §3). */
static uint64_t color_bits(tc_color_level level) {
    switch (level) {
    case TC_COLOR_LEVEL_TRUECOLOR: return TC_CAP_COLOR_16 | TC_CAP_COLOR_256 | TC_CAP_TRUECOLOR;
    case TC_COLOR_LEVEL_256:       return TC_CAP_COLOR_16 | TC_CAP_COLOR_256;
    case TC_COLOR_LEVEL_16:        return TC_CAP_COLOR_16;
    default:                       return 0;
    }
}

/* Re-derives color / max_colors from the flags bitset. */
static void caps_derive_color(tc_caps* c) {
    if (c->flags & TC_CAP_TRUECOLOR) {
        c->color      = TC_COLOR_LEVEL_TRUECOLOR;
        c->max_colors = 0;   /* truecolor: unlimited */
    } else if (c->flags & TC_CAP_COLOR_256) {
        c->color      = TC_COLOR_LEVEL_256;
        c->max_colors = 256;
    } else if (c->flags & TC_CAP_COLOR_16) {
        c->color      = TC_COLOR_LEVEL_16;
        c->max_colors = 16;
    } else {
        c->color      = TC_COLOR_LEVEL_MONO;
        c->max_colors = 0;
    }
}

/* Re-derives the mouse mode from the flags bitset (docs/03 §2: SGR -> X10 ->
 * off). */
static void caps_derive_mouse(tc_caps* c) {
    if (c->flags & TC_CAP_MOUSE_MOVE) {
        c->mouse = TC_MOUSE_MOTION;
    } else if (c->flags & TC_CAP_MOUSE_DRAG) {
        c->mouse = TC_MOUSE_DRAG;
    } else if (c->flags & TC_CAP_MOUSE_X10) {
        c->mouse = TC_MOUSE_CLICK;
    } else {
        c->mouse = TC_MOUSE_OFF;
    }
}

/* --------------------------------------------------------------------------
 * Conservative default (docs/03 §2 step 6).
 *
 * The lowest capability set that still cannot garble output: ANSI 16 colours,
 * the safe screen primitives, all SGR styles (ignored when unsupported) and
 * X10 mouse. No 256/truecolor, no focus/paste/kitty/sync, no UNICODE, no
 * PIXEL_SIZE.
 * ------------------------------------------------------------------------ */

void tc_caps_conservative_default(tc_caps* out) {
    if (!out) return;

    memset(out, 0, sizeof(*out));
    out->flags = TC_CAP_COLOR_16 | TC_CAP_ALT_SCREEN | TC_CAP_CURSOR_SHAPE
        | TC_CAP_CURSOR_HIDE | TC_CAP_TITLE | TC_CAP_STYLE_BOLD | TC_CAP_STYLE_DIM
        | TC_CAP_STYLE_ITALIC | TC_CAP_STYLE_UNDERLINE | TC_CAP_STYLE_BLINK
        | TC_CAP_STYLE_REVERSE | TC_CAP_STYLE_STRIKE | TC_CAP_MOUSE_X10;
    out->color      = TC_COLOR_LEVEL_16;
    out->max_colors = 16;
    out->mouse      = TC_MOUSE_CLICK;
}

/* --------------------------------------------------------------------------
 * Built-in TERM database (docs/03 §2 step 5, §5).
 * ------------------------------------------------------------------------ */

/* Safe screen primitives shared by the modern terminals. */
#define TC_B_SCREEN (TC_CAP_ALT_SCREEN | TC_CAP_CURSOR_SHAPE | TC_CAP_CURSOR_HIDE \
                     | TC_CAP_TITLE)
/* All SGR styles; a terminal that lacks one ignores it. */
#define TC_B_STYLES (TC_CAP_STYLE_BOLD | TC_CAP_STYLE_DIM | TC_CAP_STYLE_ITALIC \
                     | TC_CAP_STYLE_UNDERLINE | TC_CAP_STYLE_BLINK \
                     | TC_CAP_STYLE_REVERSE | TC_CAP_STYLE_STRIKE)
#define TC_B_MOUSE  (TC_CAP_MOUSE_X10 | TC_CAP_MOUSE_SGR)
/* Modern emulator baseline: screen + styles + full mouse + focus + paste. */
#define TC_B_MODERN (TC_B_SCREEN | TC_B_STYLES | TC_B_MOUSE | TC_CAP_MOUSE_DRAG \
                     | TC_CAP_MOUSE_MOVE | TC_CAP_FOCUS_EVENTS \
                     | TC_CAP_BRACKETED_PASTE | TC_CAP_UNICODE)

typedef struct tc_builtin {
    const char*    name;
    tc_color_level color;
    int32_t        max_colors;
    tc_mouse_mode  mouse;
    uint64_t       flags;   /* non-colour bits; the colour bits come from color */
} tc_builtin;

static const tc_builtin k_builtins[] = {
    /* 现代终端模拟器：真彩、完整鼠标、焦点与括号粘贴 */
    { "ms-terminal", TC_COLOR_LEVEL_TRUECOLOR, 0, TC_MOUSE_MOTION, TC_B_MODERN },
    { "wezterm",     TC_COLOR_LEVEL_TRUECOLOR, 0, TC_MOUSE_MOTION,
      TC_B_MODERN | TC_CAP_SYNC_UPDATE },
    { "alacritty",   TC_COLOR_LEVEL_TRUECOLOR, 0, TC_MOUSE_MOTION, TC_B_MODERN },
    { "kitty",       TC_COLOR_LEVEL_TRUECOLOR, 0, TC_MOUSE_MOTION,
      TC_B_MODERN | TC_CAP_SYNC_UPDATE | TC_CAP_KITTY_KEYBOARD },
    { "foot",        TC_COLOR_LEVEL_TRUECOLOR, 0, TC_MOUSE_MOTION,
      TC_B_MODERN | TC_CAP_SYNC_UPDATE },
    { "konsole",     TC_COLOR_LEVEL_TRUECOLOR, 0, TC_MOUSE_MOTION, TC_B_MODERN },
    { "gnome",       TC_COLOR_LEVEL_TRUECOLOR, 0, TC_MOUSE_MOTION, TC_B_MODERN },
    { "vscode",      TC_COLOR_LEVEL_TRUECOLOR, 0, TC_MOUSE_MOTION, TC_B_MODERN },

    /* xterm 家族：颜色保守取低位，DA / terminfo 在后续阶段升级（docs/03 §3） */
    { "xterm-256color", TC_COLOR_LEVEL_256, 256, TC_MOUSE_DRAG,
      TC_B_SCREEN | TC_B_STYLES | TC_CAP_MOUSE_X10 | TC_CAP_MOUSE_SGR
      | TC_CAP_MOUSE_DRAG | TC_CAP_FOCUS_EVENTS | TC_CAP_BRACKETED_PASTE
      | TC_CAP_MODIFY_OTHER | TC_CAP_UNICODE },
    { "xterm",      TC_COLOR_LEVEL_16, 16, TC_MOUSE_DRAG,
      TC_B_SCREEN | TC_B_STYLES | TC_CAP_MOUSE_X10 | TC_CAP_MOUSE_SGR
      | TC_CAP_MOUSE_DRAG | TC_CAP_FOCUS_EVENTS | TC_CAP_BRACKETED_PASTE
      | TC_CAP_MODIFY_OTHER | TC_CAP_UNICODE },
    { "screen-256color", TC_COLOR_LEVEL_256, 256, TC_MOUSE_DRAG,
      TC_B_SCREEN | TC_B_STYLES | TC_CAP_MOUSE_X10 | TC_CAP_MOUSE_SGR
      | TC_CAP_MOUSE_DRAG | TC_CAP_FOCUS_EVENTS | TC_CAP_BRACKETED_PASTE
      | TC_CAP_UNICODE },
    { "screen",     TC_COLOR_LEVEL_16, 16, TC_MOUSE_DRAG,
      TC_B_SCREEN | TC_B_STYLES | TC_CAP_MOUSE_X10 | TC_CAP_MOUSE_SGR
      | TC_CAP_MOUSE_DRAG | TC_CAP_FOCUS_EVENTS | TC_CAP_BRACKETED_PASTE
      | TC_CAP_UNICODE },
    { "tmux-256color", TC_COLOR_LEVEL_256, 256, TC_MOUSE_DRAG,
      TC_B_SCREEN | TC_B_STYLES | TC_CAP_MOUSE_X10 | TC_CAP_MOUSE_SGR
      | TC_CAP_MOUSE_DRAG | TC_CAP_FOCUS_EVENTS | TC_CAP_BRACKETED_PASTE
      | TC_CAP_UNICODE },
    { "tmux",       TC_COLOR_LEVEL_16, 16, TC_MOUSE_DRAG,
      TC_B_SCREEN | TC_B_STYLES | TC_CAP_MOUSE_X10 | TC_CAP_MOUSE_SGR
      | TC_CAP_MOUSE_DRAG | TC_CAP_FOCUS_EVENTS | TC_CAP_BRACKETED_PASTE
      | TC_CAP_UNICODE },

    { "cygwin",     TC_COLOR_LEVEL_16, 16, TC_MOUSE_DRAG, TC_B_MODERN },

    /* 老 / 特化终端：最低能力集 */
    { "linux",      TC_COLOR_LEVEL_16, 16, TC_MOUSE_OFF,
      TC_CAP_STYLE_BOLD | TC_CAP_STYLE_UNDERLINE | TC_CAP_STYLE_REVERSE
      | TC_CAP_UNICODE },
    { "ansi",       TC_COLOR_LEVEL_16, 16, TC_MOUSE_OFF,
      TC_CAP_STYLE_BOLD | TC_CAP_STYLE_UNDERLINE | TC_CAP_STYLE_REVERSE },
    { "vt100",      TC_COLOR_LEVEL_MONO, 0, TC_MOUSE_OFF,
      TC_CAP_STYLE_BOLD | TC_CAP_STYLE_UNDERLINE | TC_CAP_STYLE_REVERSE },
    { "dumb",       TC_COLOR_LEVEL_MONO, 0, TC_MOUSE_OFF, TC_CAP_NONE },
};

static bool builtin_name_eq(const char* a, const char* b) {
    /* strcasecmp is a BSD extension that _POSIX_C_SOURCE does not expose
     * under -std=c11; roll a small ASCII-insensitive compare instead. */
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* Looks `name` up in the built-in database; fills *out on a match. */
static bool builtin_lookup(const char* name, tc_caps* out, bool case_insensitive) {
    size_t i;

    if (!name || !*name) return false;
    for (i = 0; i < sizeof(k_builtins) / sizeof(k_builtins[0]); i++) {
        bool eq = case_insensitive
                      ? builtin_name_eq(name, k_builtins[i].name)
                      : strcmp(name, k_builtins[i].name) == 0;
        if (eq) {
            out->flags      = color_bits(k_builtins[i].color) | k_builtins[i].flags;
            out->color      = k_builtins[i].color;
            out->max_colors = k_builtins[i].max_colors;
            out->mouse      = k_builtins[i].mouse;
            return true;
        }
    }
    return false;
}

tc_status tc_caps_from_builtin(const char* term_name, tc_caps* out) {
    if (!out) return TC_ERR_INVALID_ARG;
    if (!term_name || !*term_name) return TC_ERR_UNSUPPORTED;
    return builtin_lookup(term_name, out, false) ? TC_OK : TC_ERR_UNSUPPORTED;
}

/* --------------------------------------------------------------------------
 * Environment inference (docs/03 §2 step 2, §3, §4).
 *
 * TERMCORE_CAPS (explicit bitset) wins over everything. Otherwise only the
 * colour level is decided here: TERMCORE_COLOR_LEVEL > NO_COLOR > COLORTERM >
 * TERM name; every other bit keeps the value the caller seeded (builtin or
 * default), because env inference never claims mouse/style support.
 * ------------------------------------------------------------------------ */

static bool env_full_bitset(tc_caps* out) {
    const char* s = getenv("TERMCORE_CAPS");
    char*       end;
    unsigned long long v;

    if (!s || !*s) return false;

    v = strtoull(s, &end, 0);
    if (end == s || *end != '\0') return false;   /* 非法值：忽略，继续推断 */

    out->flags = (uint64_t)v;
    caps_derive_color(out);
    caps_derive_mouse(out);
    return true;
}

static void caps_set_color_bits(tc_caps* c, tc_color_level level, int32_t max) {
    c->flags &= ~(TC_CAP_COLOR_16 | TC_CAP_COLOR_256 | TC_CAP_TRUECOLOR);
    c->flags |= color_bits(level);
    c->color      = level;
    c->max_colors = max;
}

static bool caps_parse_color_level(tc_caps* c, const char* s) {
    if (!s || !*s) return false;

    if (strcmp(s, "mono") == 0 || strcmp(s, "0") == 0) {
        caps_set_color_bits(c, TC_COLOR_LEVEL_MONO, 0);
    } else if (strcmp(s, "16") == 0) {
        caps_set_color_bits(c, TC_COLOR_LEVEL_16, 16);
    } else if (strcmp(s, "256") == 0) {
        caps_set_color_bits(c, TC_COLOR_LEVEL_256, 256);
    } else if (strcmp(s, "truecolor") == 0 || strcmp(s, "24bit") == 0) {
        caps_set_color_bits(c, TC_COLOR_LEVEL_TRUECOLOR, 0);
    } else {
        return false;   /* 非法值：忽略 */
    }
    return true;
}

static void env_apply_color(tc_caps* out) {
    const char* s;

    /* TERMCORE_COLOR_LEVEL：显式指定，最高优先级（docs/03 §4） */
    s = getenv("TERMCORE_COLOR_LEVEL");
    if (s && *s && caps_parse_color_level(out, s)) return;

    /* NO_COLOR（no-color.org）：非空且非 "0" → MONO */
    s = getenv("NO_COLOR");
    if (s && *s && strcmp(s, "0") != 0) {
        caps_set_color_bits(out, TC_COLOR_LEVEL_MONO, 0);
        return;
    }

    /* COLORTERM：truecolor / 24bit 判定真彩，优先于 TERM 的位数推断 */
    s = getenv("COLORTERM");
    if (s && (strstr(s, "truecolor") || strstr(s, "24bit"))) {
        caps_set_color_bits(out, TC_COLOR_LEVEL_TRUECOLOR, 0);
        return;
    }

    /* TERM 名称推断（docs/03 §3） */
    s = getenv("TERM");
    if (s && *s) {
        if (strstr(s, "truecolor") || strstr(s, "24bit") || strstr(s, "direct")) {
            caps_set_color_bits(out, TC_COLOR_LEVEL_TRUECOLOR, 0);
        } else if (strstr(s, "256color")) {
            caps_set_color_bits(out, TC_COLOR_LEVEL_256, 256);
        } else {
            tc_caps builtin;
            if (tc_caps_from_builtin(s, &builtin) == TC_OK) {
                caps_set_color_bits(out, builtin.color, builtin.max_colors);
            }
            /* 未知 TERM：保持保守默认 16 */
        }
    }
}

tc_status tc_caps_from_env(tc_caps* out) {
    if (!out) return TC_ERR_INVALID_ARG;

    tc_caps_conservative_default(out);
    if (env_full_bitset(out)) return TC_OK;
    env_apply_color(out);
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * Per-term state (docs/03 §2).
 * ------------------------------------------------------------------------ */

tc_status tc_caps_state_create(const tc_allocator* alloc, const tc_term_options* opt,
                               tc_caps_state** out) {
    tc_caps_state* s;
    tc_caps        builtin;
    const char*    term;
    const char*    noq;
    const char*    ci;
    bool           from_builtin = false;
    uint64_t       pre_env_flags;
    bool           env_full;

    if (!alloc || !out) return TC_ERR_INVALID_ARG;
    *out = NULL;

    s = (tc_caps_state*)alloc->alloc(alloc->ctx, sizeof(*s), sizeof(void*));
    if (!s) return TC_ERR_NOMEM;
    memset(s, 0, sizeof(*s));

    /* 第 6 档：保守默认（兜底） */
    tc_caps_conservative_default(&s->base);

    /* 第 5 档：内置 TERM 库（TERM=dumb → 最低能力集）。TERM 未匹配时再试
     * TERM_PROGRAM（大小写不敏感，docs/03 §4）。 */
    term = getenv("TERM");
    if (builtin_lookup(term, &builtin, false)) {
        s->base = builtin;
        from_builtin = true;
    } else {
        term = getenv("TERM_PROGRAM");
        if (builtin_lookup(term, &builtin, true)) {
            s->base = builtin;
            from_builtin = true;
        }
    }

    /* 第 2 档：环境推断压过内置库。TERMCORE_CAPS 整体覆盖；否则只覆盖颜色。 */
    pre_env_flags = s->base.flags;
    env_full      = env_full_bitset(&s->base);
    if (!env_full) env_apply_color(&s->base);

    /* Per-bit provenance of the detection chain (docs/03 §10.1): where each
     * bit came from and when. `base_origin` / `base_decided_at_ms` are the
     * twins that clearing an override restores exactly (docs/03 §10.2). */
    {
        int64_t now = caps_now_ms();
        int     i;

        for (i = 0; i < TC_CAP_BIT_COUNT; i++) {
            s->origin[i]     = (uint8_t)TC_CAPS_ORIGIN_DEFAULT;
            s->decided_at_ms[i] = now;
        }

        if (from_builtin) {
            for (i = 0; i < TC_CAP_BIT_COUNT; i++)
                s->origin[i] = (uint8_t)TC_CAPS_ORIGIN_BUILTIN;
        }

        if (env_full) {
            /* TERMCORE_CAPS：环境变量强制，整体标记 OVERRIDE（docs/03 §4） */
            for (i = 0; i < TC_CAP_BIT_COUNT; i++) {
                s->origin[i]     = (uint8_t)TC_CAPS_ORIGIN_OVERRIDE;
                s->decided_at_ms[i] = now;
            }
        } else {
            /* 环境推断只覆盖颜色位：与推断前不同的颜色位标 INFER。 */
            uint64_t color_bits = TC_CAP_COLOR_16 | TC_CAP_COLOR_256 | TC_CAP_TRUECOLOR;
            for (i = 0; i < TC_CAP_BIT_COUNT; i++) {
                uint64_t bit = 1ull << i;
                if ((bit & color_bits) && ((pre_env_flags ^ s->base.flags) & bit))
                    s->origin[i] = (uint8_t)TC_CAPS_ORIGIN_INFER;
            }
        }

        memcpy(s->base_origin, s->origin, sizeof(s->base_origin));
        memcpy(s->base_decided_at_ms, s->decided_at_ms,
               sizeof(s->base_decided_at_ms));
    }

    s->caps = s->base;

    /* DA 查询门控（docs/03 §6）：query_capabilities && 非 headless && 非 CI &&
     * 非 TERMCORE_NO_QUERY。阶段 D 落地查询本身。 */
    s->query_allowed = opt->query_capabilities && !opt->headless;
    noq              = getenv("TERMCORE_NO_QUERY");
    if (noq && *noq) s->query_allowed = false;
    ci = getenv("CI");
    if (ci && *ci) s->query_allowed = false;

    *out = s;
    return TC_OK;
}

void tc_caps_state_destroy(const tc_allocator* alloc, tc_caps_state* s) {
    if (!s) return;
    alloc->free(alloc->ctx, s, sizeof(*s));
}

/* --------------------------------------------------------------------------
 * Query API (docs/03 §1)
 * ------------------------------------------------------------------------ */

tc_status tc_term_get_caps(const tc_term_t* t, tc_caps* out) {
    tc_term* m;

    if (!t || !out) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    m = (tc_term*)t;
    if (!m->caps) return TC_ERR_STATE;
    *out = m->caps->caps;
    return TC_OK;
}

tc_status tc_term_get_color_level(const tc_term_t* t, tc_color_level* out) {
    tc_caps caps;
    tc_status st;

    if (!out) return TC_ERR_INVALID_ARG;
    st = tc_term_get_caps(t, &caps);
    if (st != TC_OK) return st;
    *out = caps.color;
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * Colour degradation (docs/03 §3)
 *
 * Pure mappings from a caller's colour to what the terminal can actually
 * display: TRUECOLOR keeps everything, 256 maps RGB to the nearest xterm
 * palette index, 16 maps to the nearest system colour, MONO drops the colour
 * (the style bits survive). Indexed colours pass through at TRUECOLOR / 256;
 * at 16 they are mapped through the palette to the nearest system colour.
 * ------------------------------------------------------------------------ */

/* Standard ANSI 16-colour palette (docs/03 §3). */
static const uint8_t k_system16[16][3] = {
    { 0, 0, 0 },     { 128, 0, 0 },   { 0, 128, 0 },   { 128, 128, 0 },
    { 0, 0, 128 },   { 128, 0, 128 }, { 0, 128, 128 }, { 192, 192, 192 },
    { 128, 128, 128 }, { 255, 0, 0 }, { 0, 255, 0 },   { 255, 255, 0 },
    { 0, 0, 255 },   { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 }
};

static int32_t color_sqdist(uint8_t r1, uint8_t g1, uint8_t b1,
                            uint8_t r2, uint8_t g2, uint8_t b2) {
    int32_t dr = (int32_t)r1 - (int32_t)r2;
    int32_t dg = (int32_t)g1 - (int32_t)g2;
    int32_t db = (int32_t)b1 - (int32_t)b2;
    return dr * dr + dg * dg + db * db;
}

/* xterm 256-colour palette: index -> RGB (docs/03 §3). 0-15 are the system
 * colours, 16-231 a 6x6x6 cube whose levels are 0 or 55+40*v, 232-255 a
 * 24-step gray ramp (8, 18, ..., 238). */
static void idx256_to_rgb(uint8_t idx, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (idx < 16) {
        *r = k_system16[idx][0];
        *g = k_system16[idx][1];
        *b = k_system16[idx][2];
    } else if (idx < 232) {
        int v  = idx - 16;
        int lr = v / 36;
        int lg = (v / 6) % 6;
        int lb = v % 6;
        *r = (uint8_t)(lr == 0 ? 0 : 55 + 40 * lr);
        *g = (uint8_t)(lg == 0 ? 0 : 55 + 40 * lg);
        *b = (uint8_t)(lb == 0 ? 0 : 55 + 40 * lb);
    } else {
        *r = *g = *b = (uint8_t)(8 + 10 * (idx - 232));
    }
}

/* Nearest palette entry to (r,g,b); the scan starts at index 0 so an exact
 * tie (e.g. a pure red present in both the system palette and the cube)
 * resolves to the lower index. */
static uint8_t rgb_to_256(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t best   = 0;
    int32_t best_d = INT32_MAX;

    for (int i = 0; i < 256; i++) {
        uint8_t pr, pg, pb;
        int32_t d;
        idx256_to_rgb((uint8_t)i, &pr, &pg, &pb);
        d = color_sqdist(r, g, b, pr, pg, pb);
        if (d < best_d) {
            best_d = d;
            best   = (uint8_t)i;
        }
    }
    return best;
}

static uint8_t rgb_to_16(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t best   = 0;
    int32_t best_d = INT32_MAX;

    for (int i = 0; i < 16; i++) {
        int32_t d = color_sqdist(r, g, b, k_system16[i][0], k_system16[i][1],
                                 k_system16[i][2]);
        if (d < best_d) {
            best_d = d;
            best   = (uint8_t)i;
        }
    }
    return best;
}

tc_status tc_caps_downgrade_rgb(const tc_caps* c, uint8_t r, uint8_t g, uint8_t b,
                                tc_color* out) {
    if (!c || !out) return TC_ERR_INVALID_ARG;

    switch (c->color) {
    case TC_COLOR_LEVEL_TRUECOLOR:
        *out = tc_color_rgb(r, g, b);
        break;
    case TC_COLOR_LEVEL_256:
        *out = tc_color_indexed(rgb_to_256(r, g, b));
        break;
    case TC_COLOR_LEVEL_16:
        *out = tc_color_indexed(rgb_to_16(r, g, b));
        break;
    default:   /* MONO: drop the colour, keep the style (docs/03 §3) */
        *out = tc_color_default();
        break;
    }
    return TC_OK;
}

tc_status tc_caps_downgrade_indexed(const tc_caps* c, uint8_t idx256,
                                    tc_color* out) {
    uint8_t r, g, b;

    if (!c || !out) return TC_ERR_INVALID_ARG;

    switch (c->color) {
    case TC_COLOR_LEVEL_TRUECOLOR:
    case TC_COLOR_LEVEL_256:
        /* Already supported: the index passes through unchanged. */
        *out = tc_color_indexed(idx256);
        break;
    case TC_COLOR_LEVEL_16:
        idx256_to_rgb(idx256, &r, &g, &b);
        *out = tc_color_indexed(rgb_to_16(r, g, b));
        break;
    default:   /* MONO */
        *out = tc_color_default();
        break;
    }
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * Overrides and profile writes (docs/03 §9, §10.2)
 *
 * Every write updates the *effective* state (caps / origin[] / confirmed_mask
 * / decided_at_ms[]) and marks has_override: an app write is in force. A NULL
 * override clears back to the detection-chain conclusion, whose provenance is
 * remembered in the base_ twins. After the state change, the applied runtime
 * features are replayed (该开补开，该关补关).
 * ------------------------------------------------------------------------ */

/* Index of a single capability bit, or -1 when `bit` is not one of the
 * defined bits. */
static int caps_bit_index(uint64_t bit) {
    int i;

    if (bit == 0 || (bit & (bit - 1)) != 0) return -1;
    for (i = 0; i < TC_CAP_BIT_COUNT; i++)
        if (bit & (1ull << i)) return i;
    return -1;
}

tc_status tc_term_set_caps_override(tc_term_t* t, const tc_caps* caps) {
    tc_term*      m;
    tc_caps_state* s;
    int           i;

    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    m = (tc_term*)t;
    if (!m->caps) return TC_ERR_STATE;
    s = m->caps;

    if (caps) {
        s->caps           = *caps;
        s->has_override   = true;
        s->confirmed_mask = 0;
        for (i = 0; i < TC_CAP_BIT_COUNT; i++) {
            s->origin[i]        = (uint8_t)TC_CAPS_ORIGIN_OVERRIDE;
            s->decided_at_ms[i] = caps_now_ms();
        }
    } else {
        s->caps = s->base;
        s->has_override = false;
        memcpy(s->origin, s->base_origin, sizeof(s->origin));
        memcpy(s->decided_at_ms, s->base_decided_at_ms,
               sizeof(s->decided_at_ms));
    }

    return tc_term_replay_features(t);
}

tc_status tc_term_set_caps_bits(tc_term_t* t, uint64_t clear, uint64_t set,
                                tc_caps_origin origin) {
    tc_term*       m;
    tc_caps_state* s;
    uint64_t       touched;
    int64_t        now;
    int            i;

    if (!t) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;
    if ((unsigned)origin > (unsigned)TC_CAPS_ORIGIN_PROBE)
        return TC_ERR_INVALID_ARG;

    m = (tc_term*)t;
    if (!m->caps) return TC_ERR_STATE;
    s = m->caps;

    touched = (clear | set) & TC_CAPS_BIT_MASK;
    if (touched == 0) return TC_OK;   /* nothing changes */

    s->caps.flags &= ~clear;
    s->caps.flags |= set;
    caps_derive_color(&s->caps);
    caps_derive_mouse(&s->caps);
    s->has_override   = true;
    now               = caps_now_ms();

    for (i = 0; i < TC_CAP_BIT_COUNT; i++) {
        uint64_t bit = 1ull << i;
        if (!(touched & bit)) continue;
        s->origin[i]        = (uint8_t)origin;
        s->decided_at_ms[i] = now;
        s->confirmed_mask  &= ~(1u << i);
    }
    /* A probe conclusion is user-confirmed (docs/03 §10.2, §11). */
    if (origin == TC_CAPS_ORIGIN_PROBE)
        s->confirmed_mask |= (uint32_t)(touched & TC_CAPS_BIT_MASK);

    return tc_term_replay_features(t);
}

tc_status tc_term_apply_caps_profile(tc_term_t* t, const tc_caps_profile* p) {
    tc_term*       m;
    tc_caps_state* s;
    int64_t        now;
    size_t         i;
    int            j;

    if (!t || !p) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    m = (tc_term*)t;
    if (!m->caps) return TC_ERR_STATE;
    s = m->caps;

    /* The bitset is authoritative; colour / mouse re-derive from it. Bits the
     * profile does not know fall back to unsupported (宁可低估, docs/03 §10.3). */
    s->caps.flags     = p->flags & TC_CAPS_BIT_MASK;
    caps_derive_color(&s->caps);
    caps_derive_mouse(&s->caps);
    s->has_override   = true;
    s->confirmed_mask = 0;
    now               = caps_now_ms();

    for (j = 0; j < TC_CAP_BIT_COUNT; j++) {
        s->origin[j]        = (uint8_t)TC_CAPS_ORIGIN_DEFAULT;
        s->decided_at_ms[j] = now;
    }

    for (i = 0; i < p->entry_count; i++) {
        const tc_caps_entry* e = &p->entries[i];
        int bi = caps_bit_index(e->bit);
        if (bi < 0) continue;   /* unknown / multi-bit entry: not applicable */
        s->origin[bi]        = (uint8_t)e->origin;
        s->decided_at_ms[bi] = e->decided_at_ms;
        if (e->user_confirmed) s->confirmed_mask |= (1u << bi);
    }

    return tc_term_replay_features(t);
}

/* --------------------------------------------------------------------------
 * Profile snapshot (docs/03 §10.1, §10.2)
 * ------------------------------------------------------------------------ */

tc_status tc_term_get_caps_profile(const tc_term_t* t, tc_caps_profile* out) {
    const tc_term* m;
    tc_caps_state* s;
    int            i;

    if (!t || !out) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    m = (tc_term*)t;
    if (!m->caps) return TC_ERR_STATE;
    s = m->caps;

    memset(out, 0, sizeof(*out));
    out->flags = s->caps.flags;
    out->caps  = s->caps;

    out->entries = (tc_caps_entry*)m->alloc->alloc(
        m->alloc->ctx, TC_CAP_BIT_COUNT * sizeof(tc_caps_entry), sizeof(void*));
    if (!out->entries) return TC_ERR_NOMEM;

    for (i = 0; i < TC_CAP_BIT_COUNT; i++) {
        tc_caps_entry* e = &out->entries[i];
        uint64_t bit     = 1ull << i;
        e->bit           = bit;
        e->supported     = (s->caps.flags & bit) != 0;
        e->origin        = (tc_caps_origin)s->origin[i];
        e->user_confirmed = (s->confirmed_mask & (1u << i)) != 0;
        e->decided_at_ms = s->decided_at_ms[i];
    }
    out->entry_count = TC_CAP_BIT_COUNT;
    out->alloc       = m->alloc;

    return tc_term_get_caps_fingerprint(t, &out->fp);
}

void tc_caps_profile_dispose(tc_caps_profile* p) {
    const tc_allocator* alloc;

    if (!p) return;

    alloc = p->alloc ? p->alloc : tc_allocator_default();
    if (p->entries) {
        alloc->free(alloc->ctx, p->entries,
                    p->entry_count * sizeof(tc_caps_entry));
    }
    memset(p, 0, sizeof(*p));
}

tc_status tc_caps_get_entry(const tc_caps_profile* p, uint64_t bit,
                            const tc_caps_entry** e) {
    size_t i;

    if (!p || !e) return TC_ERR_INVALID_ARG;
    *e = NULL;

    for (i = 0; i < p->entry_count; i++) {
        if (p->entries[i].bit == bit) {
            *e = &p->entries[i];
            return TC_OK;
        }
    }
    return TC_ERR_NOT_FOUND;
}

/* --------------------------------------------------------------------------
 * Environment fingerprint (docs/03 §10.1, §10.3)
 * ------------------------------------------------------------------------ */

static void copy_env(const char* name, char* dst, size_t cap) {
    const char* v = getenv(name);

    if (v) {
        strncpy(dst, v, cap - 1);
        dst[cap - 1] = '\0';
    }
}

tc_status tc_term_get_caps_fingerprint(const tc_term_t* t,
                                       tc_caps_fingerprint* out) {
    if (!t || !out) return TC_ERR_INVALID_ARG;
    if (t->state == TC_TERM_STATE_DESTROYED) return TC_ERR_STATE;

    memset(out, 0, sizeof(*out));
    copy_env("TERM", out->term, sizeof(out->term));
    copy_env("TERM_PROGRAM", out->term_program, sizeof(out->term_program));
    copy_env("COLORTERM", out->colorterm, sizeof(out->colorterm));
    out->da_primary_id   = 0;   /* stage D fills these */
    out->da_version      = 0;
    out->lib_abi_version = TERMCORE_ABI_VERSION;
    out->profile_version = TC_CAPS_PROFILE_VERSION;
    out->saved_at_ms     = caps_now_ms();
    return TC_OK;
}

/* Only the identity fields take part in the comparison; saved_at_ms does not
 * (docs/03 §10.3). A mismatch is not an error — it only means the environment
 * may have changed. */
tc_status tc_caps_profile_match(const tc_caps_profile* p,
                                const tc_caps_fingerprint* now, bool* ok) {
    if (!p || !now || !ok) return TC_ERR_INVALID_ARG;

    *ok = strcmp(p->fp.term, now->term) == 0 &&
          strcmp(p->fp.term_program, now->term_program) == 0 &&
          strcmp(p->fp.colorterm, now->colorterm) == 0 &&
          p->fp.da_primary_id == now->da_primary_id &&
          p->fp.da_version == now->da_version;
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * Serialization (docs/03 §10.3)
 *
 * Binary layout (little-endian, all fixed size):
 *   32-byte header: "TCCP" + u32 version + u64 flags + i32 color +
 *                   i32 max_colors + i32 mouse + u32 entry_count
 *   entry_count x 32-byte entries (tc_caps_entry, field by field)
 *   196-byte fingerprint
 *
 * Text layout: stable "key=value" lines, one per field; unknown keys are
 * ignored so the format can grow. The library never does file I/O.
 * ------------------------------------------------------------------------ */

#define TC_PROFILE_HEADER_SIZE 32
#define TC_PROFILE_ENTRY_SIZE  32
#define TC_PROFILE_FP_SIZE     196

static void put_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64le(uint8_t* p, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64le(const uint8_t* p) {
    uint64_t v = 0;
    int      i;
    for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

tc_status tc_caps_profile_serialize(const tc_caps_profile* p, void* buf,
                                    size_t cap, size_t* need) {
    size_t   n;
    uint8_t* o;
    size_t   i;

    if (!p || !need) return TC_ERR_INVALID_ARG;

    n    = TC_PROFILE_HEADER_SIZE + p->entry_count * TC_PROFILE_ENTRY_SIZE +
           TC_PROFILE_FP_SIZE;
    *need = n;
    if (!buf || cap == 0) return TC_OK;   /* size query */
    if (cap < n) return TC_ERR_OVERFLOW;

    o = (uint8_t*)buf;
    memcpy(o, "TCCP", 4);
    put_u32le(o + 4, TC_CAPS_PROFILE_VERSION);
    put_u64le(o + 8, p->flags);
    put_u32le(o + 16, (uint32_t)p->caps.color);
    put_u32le(o + 20, (uint32_t)p->caps.max_colors);
    put_u32le(o + 24, (uint32_t)p->caps.mouse);
    put_u32le(o + 28, (uint32_t)p->entry_count);
    o += TC_PROFILE_HEADER_SIZE;

    for (i = 0; i < p->entry_count; i++) {
        const tc_caps_entry* e = &p->entries[i];
        put_u64le(o, e->bit);
        o[8]  = e->supported ? 1 : 0;
        o[9]  = (uint8_t)e->origin;
        o[10] = e->user_confirmed ? 1 : 0;
        o[11] = 0;
        put_u64le(o + 16, (uint64_t)e->decided_at_ms);
        put_u32le(o + 24, 0);
        put_u32le(o + 28, 0);
        o += TC_PROFILE_ENTRY_SIZE;
    }

    memcpy(o, p->fp.term, sizeof(p->fp.term));
    memcpy(o + 64, p->fp.term_program, sizeof(p->fp.term_program));
    memcpy(o + 128, p->fp.colorterm, sizeof(p->fp.colorterm));
    put_u32le(o + 160, (uint32_t)p->fp.da_primary_id);
    put_u32le(o + 164, (uint32_t)p->fp.da_version);
    put_u32le(o + 168, p->fp.lib_abi_version);
    put_u64le(o + 172, p->fp.profile_version);
    put_u64le(o + 180, (uint64_t)p->fp.saved_at_ms);
    put_u32le(o + 188, 0);
    put_u32le(o + 192, 0);
    return TC_OK;
}

tc_status tc_caps_profile_deserialize(const void* buf, size_t len,
                                      tc_caps_profile* out) {
    const uint8_t* b;
    uint32_t       version;
    uint32_t       entry_count;
    size_t         need;
    size_t         i;

    if (!buf || !out) return TC_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    if (len < TC_PROFILE_HEADER_SIZE + TC_PROFILE_FP_SIZE)
        return TC_ERR_PARSE;
    b = (const uint8_t*)buf;
    if (memcmp(b, "TCCP", 4) != 0) return TC_ERR_PARSE;

    version = get_u32le(b + 4);
    if (version != TC_CAPS_PROFILE_VERSION) return TC_ERR_VERSION;

    entry_count = get_u32le(b + 28);
    if (entry_count > TC_CAP_BIT_COUNT) return TC_ERR_PARSE;
    need = TC_PROFILE_HEADER_SIZE + (size_t)entry_count * TC_PROFILE_ENTRY_SIZE +
           TC_PROFILE_FP_SIZE;
    if (len < need) return TC_ERR_PARSE;

    out->flags       = get_u64le(b + 8);
    out->caps.flags  = out->flags;
    out->caps.color  = (tc_color_level)get_u32le(b + 16);
    out->caps.max_colors = (int32_t)get_u32le(b + 20);
    out->caps.mouse  = (tc_mouse_mode)get_u32le(b + 24);
    if ((unsigned)out->caps.color > (unsigned)TC_COLOR_LEVEL_TRUECOLOR ||
        (unsigned)out->caps.mouse > (unsigned)TC_MOUSE_MOTION)
        return TC_ERR_PARSE;

    out->entry_count = entry_count;
    out->alloc       = tc_allocator_default();
    if (entry_count > 0) {
        out->entries = (tc_caps_entry*)out->alloc->alloc(
            out->alloc->ctx, entry_count * sizeof(tc_caps_entry), sizeof(void*));
        if (!out->entries) return TC_ERR_NOMEM;
    }

    b += TC_PROFILE_HEADER_SIZE;
    for (i = 0; i < entry_count; i++) {
        tc_caps_entry* e = &out->entries[i];
        e->bit           = get_u64le(b);
        e->supported     = b[8] != 0;
        e->origin        = (tc_caps_origin)b[9];
        e->user_confirmed = b[10] != 0;
        e->decided_at_ms = (int64_t)get_u64le(b + 16);
        if ((unsigned)e->origin > (unsigned)TC_CAPS_ORIGIN_PROBE) {
            tc_caps_profile_dispose(out);
            return TC_ERR_PARSE;
        }
        b += TC_PROFILE_ENTRY_SIZE;
    }

    memcpy(out->fp.term, b, sizeof(out->fp.term));
    memcpy(out->fp.term_program, b + 64, sizeof(out->fp.term_program));
    memcpy(out->fp.colorterm, b + 128, sizeof(out->fp.colorterm));
    out->fp.da_primary_id   = (int32_t)get_u32le(b + 160);
    out->fp.da_version      = (int32_t)get_u32le(b + 164);
    out->fp.lib_abi_version = get_u32le(b + 168);
    out->fp.profile_version = get_u64le(b + 172);
    out->fp.saved_at_ms     = (int64_t)get_u64le(b + 180);
    return TC_OK;
}

/* --------------------------------------------------------------------------
 * Text serialization (docs/03 §10.3): stable key=value lines.
 * ------------------------------------------------------------------------ */

static const char* origin_to_name(tc_caps_origin o) {
    switch (o) {
    case TC_CAPS_ORIGIN_DEFAULT:  return "default";
    case TC_CAPS_ORIGIN_BUILTIN:  return "builtin";
    case TC_CAPS_ORIGIN_TERMINFO: return "terminfo";
    case TC_CAPS_ORIGIN_INFER:    return "infer";
    case TC_CAPS_ORIGIN_QUERY:    return "query";
    case TC_CAPS_ORIGIN_OVERRIDE: return "override";
    case TC_CAPS_ORIGIN_PROBE:    return "probe";
    default:                      return "unknown";
    }
}

static bool origin_from_name(const char* s, tc_caps_origin* out) {
    if (strcmp(s, "default") == 0)  { *out = TC_CAPS_ORIGIN_DEFAULT;  return true; }
    if (strcmp(s, "builtin") == 0)  { *out = TC_CAPS_ORIGIN_BUILTIN;  return true; }
    if (strcmp(s, "terminfo") == 0) { *out = TC_CAPS_ORIGIN_TERMINFO; return true; }
    if (strcmp(s, "infer") == 0)    { *out = TC_CAPS_ORIGIN_INFER;    return true; }
    if (strcmp(s, "query") == 0)    { *out = TC_CAPS_ORIGIN_QUERY;    return true; }
    if (strcmp(s, "override") == 0) { *out = TC_CAPS_ORIGIN_OVERRIDE; return true; }
    if (strcmp(s, "probe") == 0)    { *out = TC_CAPS_ORIGIN_PROBE;    return true; }
    return false;
}

static const char* color_to_name(tc_color_level c) {
    switch (c) {
    case TC_COLOR_LEVEL_MONO:      return "mono";
    case TC_COLOR_LEVEL_16:        return "16";
    case TC_COLOR_LEVEL_256:       return "256";
    case TC_COLOR_LEVEL_TRUECOLOR: return "truecolor";
    default:                       return "unknown";
    }
}

static bool color_from_name(const char* s, tc_color_level* out) {
    if (strcmp(s, "mono") == 0)     { *out = TC_COLOR_LEVEL_MONO;      return true; }
    if (strcmp(s, "16") == 0)       { *out = TC_COLOR_LEVEL_16;        return true; }
    if (strcmp(s, "256") == 0)      { *out = TC_COLOR_LEVEL_256;       return true; }
    if (strcmp(s, "truecolor") == 0 || strcmp(s, "24bit") == 0) {
        *out = TC_COLOR_LEVEL_TRUECOLOR;
        return true;
    }
    return false;
}

static const char* mouse_to_name(tc_mouse_mode m) {
    switch (m) {
    case TC_MOUSE_OFF:    return "off";
    case TC_MOUSE_CLICK:  return "click";
    case TC_MOUSE_DRAG:   return "drag";
    case TC_MOUSE_MOTION: return "motion";
    default:              return "unknown";
    }
}

static bool mouse_from_name(const char* s, tc_mouse_mode* out) {
    if (strcmp(s, "off") == 0)    { *out = TC_MOUSE_OFF;    return true; }
    if (strcmp(s, "click") == 0)  { *out = TC_MOUSE_CLICK;  return true; }
    if (strcmp(s, "drag") == 0)   { *out = TC_MOUSE_DRAG;   return true; }
    if (strcmp(s, "motion") == 0) { *out = TC_MOUSE_MOTION; return true; }
    return false;
}

/* Grow-only text writer: with buf == NULL it only counts; with a buffer it
 * appends in place (the caller has already sized it via the count pass). */
typedef struct text_writer {
    char*  buf;
    size_t cap;
    size_t len;
} text_writer;

static void tw_printf(text_writer* w, const char* fmt, ...) {
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    if (w->buf) {
        if (w->len + (size_t)n + 1 <= w->cap) {
            va_start(ap, fmt);
            vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
            va_end(ap);
        }
    }
    w->len += (size_t)n;
}

static void emit_profile_text(text_writer* w, const tc_caps_profile* p) {
    size_t i;

    tw_printf(w, "version=%u\n", (unsigned)TC_CAPS_PROFILE_VERSION);
    tw_printf(w, "flags=0x%llx\n", (unsigned long long)p->flags);
    tw_printf(w, "color=%s\n", color_to_name(p->caps.color));
    tw_printf(w, "max_colors=%d\n", (int)p->caps.max_colors);
    tw_printf(w, "mouse=%s\n", mouse_to_name(p->caps.mouse));
    for (i = 0; i < p->entry_count; i++) {
        const tc_caps_entry* e = &p->entries[i];
        tw_printf(w, "entry=0x%llx:%d:%s:%d:%lld\n",
                  (unsigned long long)e->bit, e->supported ? 1 : 0,
                  origin_to_name(e->origin), e->user_confirmed ? 1 : 0,
                  (long long)e->decided_at_ms);
    }
    tw_printf(w, "fp_term=%s\n", p->fp.term);
    tw_printf(w, "fp_term_program=%s\n", p->fp.term_program);
    tw_printf(w, "fp_colorterm=%s\n", p->fp.colorterm);
    tw_printf(w, "fp_da_id=%d\n", (int)p->fp.da_primary_id);
    tw_printf(w, "fp_da_version=%d\n", (int)p->fp.da_version);
    tw_printf(w, "fp_abi=%u\n", (unsigned)p->fp.lib_abi_version);
    tw_printf(w, "fp_profile=%llu\n", (unsigned long long)p->fp.profile_version);
    tw_printf(w, "fp_saved_at=%lld\n", (long long)p->fp.saved_at_ms);
}

tc_status tc_caps_profile_to_text(const tc_caps_profile* p, char* buf,
                                  size_t cap, size_t* need) {
    text_writer w;

    if (!p || !need) return TC_ERR_INVALID_ARG;

    w.buf = NULL;
    w.cap = 0;
    w.len = 0;
    emit_profile_text(&w, p);

    *need = w.len + 1;   /* + NUL */
    if (!buf || cap == 0) return TC_OK;
    if (cap < w.len + 1) return TC_ERR_OVERFLOW;

    w.buf = buf;
    w.cap = cap;
    w.len = 0;
    emit_profile_text(&w, p);
    buf[w.len] = '\0';
    return TC_OK;
}

/* Appends one parsed entry; a duplicate bit overwrites the earlier entry. */
static tc_status text_add_entry(tc_caps_profile* p, const tc_caps_entry* e) {
    tc_caps_entry* ne;
    size_t         i;

    for (i = 0; i < p->entry_count; i++) {
        if (p->entries[i].bit == e->bit) {
            p->entries[i] = *e;
            return TC_OK;
        }
    }

    ne = (tc_caps_entry*)p->alloc->realloc(
        p->alloc->ctx, p->entries, p->entry_count * sizeof(tc_caps_entry),
        (p->entry_count + 1) * sizeof(tc_caps_entry), sizeof(void*));
    if (!ne) return TC_ERR_NOMEM;
    p->entries      = ne;
    p->entries[p->entry_count] = *e;
    p->entry_count++;
    return TC_OK;
}

/* Copies a value bounded by the end of its line, NUL-terminated. */
static void copy_text_value(char* dst, size_t cap, const char* v,
                            const char* line_end) {
    size_t n = (size_t)(line_end - v);

    if (n >= cap) n = cap - 1;
    memcpy(dst, v, n);
    dst[n] = '\0';
}

/* Parses one "entry=" value: bit:supported:origin:confirmed:decided. */
static tc_status text_parse_entry(const char* v, tc_caps_entry* out) {
    const char* p = v;
    char*       ep;
    unsigned long long bit;
    long long          decided;

    bit = strtoull(p, &ep, 0);
    if (ep == p || *ep != ':') return TC_ERR_PARSE;
    out->bit = bit;
    p = ep + 1;

    if (*p != '0' && *p != '1') return TC_ERR_PARSE;
    out->supported = *p == '1';
    p++;
    if (*p != ':') return TC_ERR_PARSE;
    p++;

    {
        char name[16];
        size_t n = 0;
        while (*p && *p != ':' && n + 1 < sizeof(name)) name[n++] = *p++;
        name[n] = '\0';
        if (*p != ':' || !origin_from_name(name, &out->origin))
            return TC_ERR_PARSE;
    }
    p++;

    if (*p != '0' && *p != '1') return TC_ERR_PARSE;
    out->user_confirmed = *p == '1';
    p++;
    if (*p != ':') return TC_ERR_PARSE;
    p++;

    decided = strtoll(p, &ep, 10);
    if (ep == p) return TC_ERR_PARSE;
    out->decided_at_ms = decided;
    return TC_OK;
}

tc_status tc_caps_profile_from_text(const char* text, size_t len,
                                    tc_caps_profile* out) {
    const char* p;
    const char* end;
    bool        have_version = false;
    bool        have_flags   = false;
    bool        have_derived = false;
    tc_status   st           = TC_OK;

    if (!text || !out) return TC_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->alloc = tc_allocator_default();

    p   = text;
    end = text + len;
    while (p < end) {
        const char* eol = (const char*)memchr(p, '\n', (size_t)(end - p));
        const char* line_end = eol ? eol : end;
        const char* eq;
        const char* v;
        size_t      klen;
        char        key[32];

        /* Skip empty lines. */
        if (line_end == p || (line_end - p == 1 && p[0] == '\r')) {
            p = eol ? eol + 1 : end;
            continue;
        }

        /* Drop a trailing CR (CRLF input). */
        if (line_end > p && line_end[-1] == '\r') line_end--;

        eq = (const char*)memchr(p, '=', (size_t)(line_end - p));
        if (!eq) {
            st = TC_ERR_PARSE;
            break;
        }
        klen = (size_t)(eq - p);
        if (klen == 0 || klen >= sizeof(key)) {
            st = TC_ERR_PARSE;
            break;
        }
        memcpy(key, p, klen);
        key[klen] = '\0';
        v = eq + 1;

        if (strcmp(key, "version") == 0) {
            char* ep;
            unsigned long vv = strtoul(v, &ep, 10);
            if (ep == v || ep != line_end) { st = TC_ERR_PARSE; break; }
            if (vv != TC_CAPS_PROFILE_VERSION) { st = TC_ERR_VERSION; break; }
            have_version = true;
        } else if (strcmp(key, "flags") == 0) {
            char* ep;
            out->flags = strtoull(v, &ep, 0);
            if (ep == v || ep != line_end) { st = TC_ERR_PARSE; break; }
            out->caps.flags = out->flags;
            have_flags = true;
        } else if (strcmp(key, "color") == 0) {
            tc_color_level c;
            char           name[32];
            size_t         nv = (size_t)(line_end - v);
            if (nv >= sizeof(name)) nv = sizeof(name) - 1;
            memcpy(name, v, nv);
            name[nv] = '\0';
            if (!color_from_name(name, &c)) { st = TC_ERR_PARSE; break; }
            out->caps.color = c;
            have_derived = true;
        } else if (strcmp(key, "max_colors") == 0) {
            char* ep;
            out->caps.max_colors = (int32_t)strtol(v, &ep, 10);
            if (ep == v || ep != line_end) { st = TC_ERR_PARSE; break; }
            have_derived = true;
        } else if (strcmp(key, "mouse") == 0) {
            tc_mouse_mode m;
            char          name[32];
            size_t        nv = (size_t)(line_end - v);
            if (nv >= sizeof(name)) nv = sizeof(name) - 1;
            memcpy(name, v, nv);
            name[nv] = '\0';
            if (!mouse_from_name(name, &m)) { st = TC_ERR_PARSE; break; }
            out->caps.mouse = m;
            have_derived = true;
        } else if (strcmp(key, "entry") == 0) {
            tc_caps_entry e;
            memset(&e, 0, sizeof(e));
            st = text_parse_entry(v, &e);
            if (st != TC_OK) break;
            st = text_add_entry(out, &e);
            if (st != TC_OK) break;
        } else if (strcmp(key, "fp_term") == 0) {
            copy_text_value(out->fp.term, sizeof(out->fp.term), v, line_end);
        } else if (strcmp(key, "fp_term_program") == 0) {
            copy_text_value(out->fp.term_program, sizeof(out->fp.term_program),
                            v, line_end);
        } else if (strcmp(key, "fp_colorterm") == 0) {
            copy_text_value(out->fp.colorterm, sizeof(out->fp.colorterm),
                            v, line_end);
        } else if (strcmp(key, "fp_da_id") == 0) {
            char* ep;
            out->fp.da_primary_id = (int32_t)strtol(v, &ep, 10);
            if (ep == v || ep != line_end) { st = TC_ERR_PARSE; break; }
        } else if (strcmp(key, "fp_da_version") == 0) {
            char* ep;
            out->fp.da_version = (int32_t)strtol(v, &ep, 10);
            if (ep == v || ep != line_end) { st = TC_ERR_PARSE; break; }
        } else if (strcmp(key, "fp_abi") == 0) {
            char* ep;
            out->fp.lib_abi_version = (uint32_t)strtoul(v, &ep, 10);
            if (ep == v || ep != line_end) { st = TC_ERR_PARSE; break; }
        } else if (strcmp(key, "fp_profile") == 0) {
            char* ep;
            out->fp.profile_version = strtoull(v, &ep, 10);
            if (ep == v || ep != line_end) { st = TC_ERR_PARSE; break; }
        } else if (strcmp(key, "fp_saved_at") == 0) {
            char* ep;
            out->fp.saved_at_ms = (int64_t)strtoll(v, &ep, 10);
            if (ep == v || ep != line_end) { st = TC_ERR_PARSE; break; }
        }
        /* Unknown keys are ignored so the format can grow (docs/03 §10.3). */

        p = eol ? eol + 1 : end;
    }

    if (st != TC_OK || !have_version || !have_flags) {
        if (st == TC_OK) st = TC_ERR_PARSE;   /* missing required keys */
        tc_caps_profile_dispose(out);
        return st;
    }

    /* Derived fields fall back to the bitset when the text omitted them. */
    if (!have_derived) {
        caps_derive_color(&out->caps);
        caps_derive_mouse(&out->caps);
    }
    return TC_OK;
}
