#include <caps/caps_internal.h>

#include <control/term_internal.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

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
    } else {
        term = getenv("TERM_PROGRAM");
        if (builtin_lookup(term, &builtin, true)) s->base = builtin;
    }

    /* 第 2 档：环境推断压过内置库。TERMCORE_CAPS 整体覆盖；否则只覆盖颜色。 */
    if (env_full_bitset(&s->base)) {
        /* 位集已整体覆盖，颜色 / 鼠标已随之推导 */
    } else {
        env_apply_color(&s->base);
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
