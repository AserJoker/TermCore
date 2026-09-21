/* Input layer tests (docs/04). Every case runs headless on the null backend
 * and feeds bytes through the real parser via tc_term_inject_input, or events
 * via tc_term_inject_event — the same code path a real terminal takes. */
#include <gtest/gtest.h>

#include <termcore/tc.h>

namespace {

tc_term_options input_options(const tc_allocator* alloc = nullptr) {
    tc_term_options opt;
    tc_init_term_options(&opt);
    opt.headless  = true;
    opt.allocator = alloc;
    return opt;
}

/* Creates an active headless session. */
tc_term_t* make_term(tc_term_options opt = input_options()) {
    tc_term_t* t = nullptr;
    EXPECT_EQ(tc_term_create(&opt, &t), TC_OK);
    EXPECT_NE(t, nullptr);
    EXPECT_EQ(tc_term_enter(t), TC_OK);
    return t;
}

tc_event take_one(tc_term_t* t, bool expect = true) {
    tc_event ev;
    bool     has = false;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    if (expect) {
        EXPECT_TRUE(has);
    } else {
        EXPECT_FALSE(has);
    }
    return ev;
}

tc_event make_key_event(uint32_t codepoint, tc_key key, uint16_t mods) {
    tc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind            = TC_EV_KEY;
    ev.u.key.codepoint = codepoint;
    ev.u.key.key       = key;
    ev.u.key.mods      = mods;
    return ev;
}

}  // namespace

/* --------------------------------------------------------------------------
 * Pull primitives: state and ordering (docs/04 §4)
 * ------------------------------------------------------------------------ */

TEST(InputPull, GetWithoutEnterIsStateError) {
    tc_term_options opt = input_options();
    tc_term_t*      t   = nullptr;
    ASSERT_EQ(tc_term_create(&opt, &t), TC_OK);

    tc_event ev;
    bool     has = false;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_ERR_STATE);
    EXPECT_EQ(tc_peek_event(t, &ev, &has), TC_ERR_STATE);
    EXPECT_EQ(tc_wait_event(t, 0, &has), TC_ERR_STATE);
    EXPECT_EQ(tc_term_flush_pending(t, &has), TC_ERR_STATE);

    tc_term_destroy(t);
}

TEST(InputPull, GetOnEmptyQueueReportsNoEvent) {
    tc_term_t* t = make_term();
    tc_event   ev;
    bool       has = true;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);
    tc_term_destroy(t);
}

TEST(InputPull, InjectedEventsRoundTripInOrder) {
    tc_term_t* t = make_term();

    tc_event a = make_key_event('a', TC_KEY_NONE, 0);
    tc_event b = make_key_event('b', TC_KEY_NONE, TC_MOD_CTRL);
    EXPECT_EQ(tc_term_inject_event(t, &a), TC_OK);
    EXPECT_EQ(tc_term_inject_event(t, &b), TC_OK);

    tc_event got = take_one(t);
    EXPECT_EQ(got.kind, TC_EV_KEY);
    EXPECT_EQ(got.u.key.codepoint, 'a');
    EXPECT_EQ(got.u.key.mods, 0);

    got = take_one(t);
    EXPECT_EQ(got.u.key.codepoint, 'b');
    EXPECT_EQ(got.u.key.mods, TC_MOD_CTRL);

    tc_term_destroy(t);
}

TEST(InputPull, PeekDoesNotConsume) {
    tc_term_t* t = make_term();

    tc_event a = make_key_event('x', TC_KEY_NONE, 0);
    EXPECT_EQ(tc_term_inject_event(t, &a), TC_OK);

    tc_event ev;
    bool     has = false;
    EXPECT_EQ(tc_peek_event(t, &ev, &has), TC_OK);
    EXPECT_TRUE(has);
    EXPECT_EQ(ev.u.key.codepoint, 'x');

    /* Still there after peek. */
    EXPECT_EQ(tc_peek_event(t, &ev, &has), TC_OK);
    EXPECT_TRUE(has);

    /* And get consumes it. */
    ev = take_one(t);
    EXPECT_EQ(ev.u.key.codepoint, 'x');
    tc_term_destroy(t);
}

TEST(InputPull, InvalidInjectedEventKindIsRejected) {
    tc_term_t* t = make_term();

    tc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = static_cast<tc_event_kind>(99);
    EXPECT_EQ(tc_term_inject_event(t, &ev), TC_ERR_INVALID_ARG);
    ev.kind = static_cast<tc_event_kind>(0);
    EXPECT_EQ(tc_term_inject_event(t, &ev), TC_ERR_INVALID_ARG);

    tc_term_destroy(t);
}

TEST(InputPull, InjectWithoutEnterIsStateError) {
    tc_term_options opt = input_options();
    tc_term_t*      t   = nullptr;
    ASSERT_EQ(tc_term_create(&opt, &t), TC_OK);

    tc_event ev = make_key_event('a', TC_KEY_NONE, 0);
    EXPECT_EQ(tc_term_inject_event(t, &ev), TC_ERR_STATE);
    EXPECT_EQ(tc_term_inject_input(t, "a", 1), TC_ERR_STATE);
    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Event queue and overflow (docs/04 §13)
 * ------------------------------------------------------------------------ */

TEST(InputQueue, OverflowDropOldestKeepsNewest) {
    tc_term_options opt = input_options();
    opt.event_queue_capacity = 4;
    tc_term_t* t = make_term(opt);

    for (int i = 0; i < 6; i++) {
        tc_event ev = make_key_event(static_cast<uint32_t>('0' + i), TC_KEY_NONE, 0);
        EXPECT_EQ(tc_term_inject_event(t, &ev), TC_OK);
    }

    /* 6 injected into a queue of 4 (DROP_OLDEST default): the two oldest are
     * gone, events "2".."5" survive in order. */
    for (char c = '2'; c <= '5'; c++) {
        tc_event ev = take_one(t);
        EXPECT_EQ(ev.u.key.codepoint, static_cast<uint32_t>(c));
    }

    tc_event ev;
    bool     has = true;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);
    tc_term_destroy(t);
}

TEST(InputQueue, OverflowDropNewestReportsOverflow) {
    tc_term_options opt = input_options();
    opt.event_queue_capacity = 4;
    opt.overflow             = TC_OVERFLOW_DROP_NEWEST;
    tc_term_t* t = make_term(opt);

    for (int i = 0; i < 4; i++) {
        tc_event ev = make_key_event(static_cast<uint32_t>('0' + i), TC_KEY_NONE, 0);
        EXPECT_EQ(tc_term_inject_event(t, &ev), TC_OK);
    }
    tc_event ev = make_key_event('X', TC_KEY_NONE, 0);
    EXPECT_EQ(tc_term_inject_event(t, &ev), TC_OK);   /* fits the inject buffer */

    /* The fifth event cannot enter the 4-slot queue: the pull reports
     * TC_ERR_OVERFLOW but still delivers the surviving events. */
    tc_event got;
    bool     has = false;
    EXPECT_EQ(tc_get_event(t, &got, &has), TC_ERR_OVERFLOW);
    EXPECT_TRUE(has);
    EXPECT_EQ(got.u.key.codepoint, '0');

    for (char c = '1'; c <= '3'; c++) {
        got = take_one(t);
        EXPECT_EQ(got.u.key.codepoint, static_cast<uint32_t>(c));
    }
    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Injection and source takeover (docs/04 §5)
 * ------------------------------------------------------------------------ */

namespace {

struct script_source {
    const char* data;
    size_t      len;
    size_t      pos;
    int         dispose_calls = 0;
};

tc_status script_wait_ready(void* ctx, int32_t timeout_ms, bool* ready) {
    script_source* s = static_cast<script_source*>(ctx);
    (void)timeout_ms;
    if (ready) *ready = s->pos < s->len;
    return TC_OK;
}

tc_status script_read(void* ctx, void* buf, size_t cap, size_t* nread) {
    script_source* s  = static_cast<script_source*>(ctx);
    size_t         n  = s->len - s->pos;
    if (n > cap) n = cap;
    if (n == 0) {
        if (nread) *nread = 0;
        return TC_OK;
    }
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    if (nread) *nread = n;
    return TC_OK;
}

void script_dispose(void* ctx) {
    script_source* s = static_cast<script_source*>(ctx);
    s->dispose_calls++;
}

const tc_input_source_vtable kScriptVtable = {
    script_wait_ready,
    script_read,
    script_dispose,
};

}  // namespace

TEST(InputSource, ExternalSourceFeedsEvents) {
    tc_term_t* t = make_term();

    script_source src;
    memset(&src, 0, sizeof(src));
    src.data = "ab";
    src.len  = 2;

    EXPECT_EQ(tc_term_set_input_source(t, &kScriptVtable, &src), TC_OK);

    /* The parser is not implemented yet (next milestone): bytes from the
     * source are consumed by the greedy read, so nothing is produced yet. */
    (void)src;
    tc_term_destroy(t);
}

TEST(InputSource, RestoringNullSourceDisposesPrevious) {
    tc_term_t* t = make_term();

    script_source src;
    memset(&src, 0, sizeof(src));
    EXPECT_EQ(tc_term_set_input_source(t, &kScriptVtable, &src), TC_OK);
    EXPECT_EQ(tc_term_set_input_source(t, nullptr, nullptr), TC_OK);
    EXPECT_EQ(src.dispose_calls, 1);

    tc_term_destroy(t);
}

TEST(InputSource, SourceDisposedOnDestroy) {
    tc_term_t* t = make_term();

    script_source src;
    memset(&src, 0, sizeof(src));
    EXPECT_EQ(tc_term_set_input_source(t, &kScriptVtable, &src), TC_OK);
    EXPECT_EQ(src.dispose_calls, 0);

    tc_term_destroy(t);
    EXPECT_EQ(src.dispose_calls, 1);
}

/* --------------------------------------------------------------------------
 * Diagnostics (docs/04 §16)
 * ------------------------------------------------------------------------ */

TEST(InputDiag, KeyNameRoundTrip) {
    const char* name = nullptr;
    EXPECT_EQ(tc_key_name(TC_KEY_UP, &name), TC_OK);
    EXPECT_STREQ(name, "UP");
    EXPECT_EQ(tc_key_name(TC_KEY_F1, &name), TC_OK);
    EXPECT_STREQ(name, "F1");
    EXPECT_EQ(tc_key_name(TC_KEY_NONE, &name), TC_ERR_INVALID_ARG);
    EXPECT_EQ(tc_key_name(static_cast<tc_key>(999), &name), TC_ERR_INVALID_ARG);
}

TEST(InputDiag, ModsNameComposes) {
    char buf[64];
    EXPECT_EQ(tc_mods_name(TC_MOD_CTRL | TC_MOD_ALT, buf, sizeof(buf)), TC_OK);
    EXPECT_STREQ(buf, "CTRL+ALT");
    EXPECT_EQ(tc_mods_name(0, buf, sizeof(buf)), TC_OK);
    EXPECT_STREQ(buf, "");
    EXPECT_EQ(tc_mods_name(TC_MOD_SHIFT, buf, sizeof(buf)), TC_OK);
    EXPECT_STREQ(buf, "SHIFT");
    EXPECT_EQ(tc_mods_name(TC_MOD_SHIFT, buf, 2), TC_ERR_OVERFLOW);
}

/* --------------------------------------------------------------------------
 * Parser: basic keys (docs/04 §6, §8)
 * ------------------------------------------------------------------------ */

/* Feeds bytes through the real parser and returns the first event taken. */
tc_event parse_one(tc_term_t* t, const char* bytes) {
    EXPECT_EQ(tc_term_inject_input(t, bytes, strlen(bytes)), TC_OK);
    return take_one(t);
}

/* Explicit-length variant for payloads containing NUL bytes. */
tc_event parse_one_n(tc_term_t* t, const char* bytes, size_t len) {
    EXPECT_EQ(tc_term_inject_input(t, bytes, len), TC_OK);
    return take_one(t);
}

TEST(Parser, PrintableAsciiKey) {
    tc_term_t* t = make_term();

    tc_event ev = parse_one(t, "a");
    EXPECT_EQ(ev.kind, TC_EV_KEY);
    EXPECT_EQ(ev.u.key.codepoint, 'a');
    EXPECT_EQ(ev.u.key.key, TC_KEY_NONE);
    EXPECT_EQ(ev.u.key.mods, 0);

    tc_term_destroy(t);
}

TEST(Parser, CtrlLetterKey) {
    tc_term_t* t = make_term();

    /* 0x01 = Ctrl+A. */
    tc_event ev = parse_one(t, "\x01");
    EXPECT_EQ(ev.u.key.codepoint, 'a');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_CTRL);

    /* 0x1A = Ctrl+Z. */
    ev = parse_one(t, "\x1a");
    EXPECT_EQ(ev.u.key.codepoint, 'z');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_CTRL);

    tc_term_destroy(t);
}

TEST(Parser, ControlKeys) {
    tc_term_t* t = make_term();

    tc_event ev = parse_one(t, "\r");
    EXPECT_EQ(ev.u.key.key, TC_KEY_ENTER);
    EXPECT_EQ(ev.u.key.codepoint, 0);

    ev = parse_one(t, "\t");
    EXPECT_EQ(ev.u.key.key, TC_KEY_TAB);

    ev = parse_one(t, "\x08");
    EXPECT_EQ(ev.u.key.key, TC_KEY_BACKSPACE);

    ev = parse_one(t, "\x7f");
    EXPECT_EQ(ev.u.key.key, TC_KEY_BACKSPACE);

    /* LF = Ctrl+J. */
    ev = parse_one(t, "\n");
    EXPECT_EQ(ev.u.key.codepoint, 'j');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_CTRL);

    /* NUL = Ctrl+Space. */
    ev = parse_one_n(t, "\x00", 1);
    EXPECT_EQ(ev.u.key.codepoint, ' ');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_CTRL);

    tc_term_destroy(t);
}

TEST(Parser, EscapeKeyIsNotImmediatelyEmitted) {
    tc_term_t* t = make_term();

    /* A lone ESC is not settled by the parser itself (no deadline here). */
    EXPECT_EQ(tc_term_inject_input(t, "\x1b", 1), TC_OK);
    tc_event ev;
    bool     has = false;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Parser: arrows, editing and function keys (docs/04 §8.1)
 * ------------------------------------------------------------------------ */

TEST(Parser, ArrowsCsiAndSs3) {
    tc_term_t* t = make_term();

    tc_event ev = parse_one(t, "\x1b[A");
    EXPECT_EQ(ev.u.key.key, TC_KEY_UP);
    ev = parse_one(t, "\x1b[B");
    EXPECT_EQ(ev.u.key.key, TC_KEY_DOWN);
    ev = parse_one(t, "\x1b[C");
    EXPECT_EQ(ev.u.key.key, TC_KEY_RIGHT);
    ev = parse_one(t, "\x1b[D");
    EXPECT_EQ(ev.u.key.key, TC_KEY_LEFT);

    /* SS3 forms. */
    ev = parse_one(t, "\x1bOA");
    EXPECT_EQ(ev.u.key.key, TC_KEY_UP);

    tc_term_destroy(t);
}

TEST(Parser, ModifiedArrowsUseXtermModifierParam) {
    tc_term_t* t = make_term();

    /* CSI 1 ; 2 A = Shift+Up. */
    tc_event ev = parse_one(t, "\x1b[1;2A");
    EXPECT_EQ(ev.u.key.key, TC_KEY_UP);
    EXPECT_EQ(ev.u.key.mods, TC_MOD_SHIFT);

    /* 5 = Ctrl. */
    ev = parse_one(t, "\x1b[1;5C");
    EXPECT_EQ(ev.u.key.key, TC_KEY_RIGHT);
    EXPECT_EQ(ev.u.key.mods, TC_MOD_CTRL);

    /* 3 = Alt. */
    ev = parse_one(t, "\x1b[1;3D");
    EXPECT_EQ(ev.u.key.key, TC_KEY_LEFT);
    EXPECT_EQ(ev.u.key.mods, TC_MOD_ALT);

    tc_term_destroy(t);
}

TEST(Parser, EditingKeysTilde) {
    tc_term_t* t = make_term();

    struct { const char* seq; tc_key key; } kCases[] = {
        { "\x1b[1~", TC_KEY_HOME },    { "\x1b[2~", TC_KEY_INSERT },
        { "\x1b[3~", TC_KEY_DELETE },  { "\x1b[4~", TC_KEY_END },
        { "\x1b[5~", TC_KEY_PAGE_UP }, { "\x1b[6~", TC_KEY_PAGE_DOWN },
        { "\x1b[7~", TC_KEY_HOME },    { "\x1b[8~", TC_KEY_END },
    };
    for (auto& c : kCases) {
        tc_event ev = parse_one(t, c.seq);
        EXPECT_EQ(ev.u.key.key, c.key) << "seq: " << c.seq;
    }

    tc_term_destroy(t);
}

TEST(Parser, FunctionKeys) {
    tc_term_t* t = make_term();

    struct { const char* seq; tc_key key; } kCases[] = {
        { "\x1b[11~", TC_KEY_F1 },  { "\x1b[12~", TC_KEY_F2 },
        { "\x1b[13~", TC_KEY_F3 },  { "\x1b[14~", TC_KEY_F4 },
        { "\x1b[15~", TC_KEY_F5 },  { "\x1b[17~", TC_KEY_F6 },
        { "\x1b[18~", TC_KEY_F7 },  { "\x1b[19~", TC_KEY_F8 },
        { "\x1b[20~", TC_KEY_F9 },  { "\x1b[21~", TC_KEY_F10 },
        { "\x1b[23~", TC_KEY_F11 }, { "\x1b[24~", TC_KEY_F12 },
    };
    for (auto& c : kCases) {
        tc_event ev = parse_one(t, c.seq);
        EXPECT_EQ(ev.u.key.key, c.key) << "seq: " << c.seq;
    }

    /* SS3 F1-F4. */
    tc_event ev = parse_one(t, "\x1bOP");
    EXPECT_EQ(ev.u.key.key, TC_KEY_F1);
    ev = parse_one(t, "\x1bOQ");
    EXPECT_EQ(ev.u.key.key, TC_KEY_F2);

    tc_term_destroy(t);
}

TEST(Parser, HomeEndAndShiftTab) {
    tc_term_t* t = make_term();

    tc_event ev = parse_one(t, "\x1b[H");
    EXPECT_EQ(ev.u.key.key, TC_KEY_HOME);
    ev = parse_one(t, "\x1b[F");
    EXPECT_EQ(ev.u.key.key, TC_KEY_END);
    ev = parse_one(t, "\x1b[Z");
    EXPECT_EQ(ev.u.key.key, TC_KEY_TAB);
    EXPECT_EQ(ev.u.key.mods, TC_MOD_SHIFT);
    ev = parse_one(t, "\x1bOH");
    EXPECT_EQ(ev.u.key.key, TC_KEY_HOME);

    tc_term_destroy(t);
}

TEST(Parser, AltPrintableCharacter) {
    tc_term_t* t = make_term();

    tc_event ev = parse_one(t, "\x1b" "a");
    EXPECT_EQ(ev.u.key.codepoint, 'a');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_ALT);

    ev = parse_one(t, "\x1b\x20");
    EXPECT_EQ(ev.u.key.codepoint, ' ');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_ALT);

    tc_term_destroy(t);
}

TEST(Parser, EscEscSettlesFirstEscImmediately) {
    tc_term_t* t = make_term();

    /* "\x1b\x1b[A": first ESC -> ESC key, then Up. */
    EXPECT_EQ(tc_term_inject_input(t, "\x1b\x1b[A", 4), TC_OK);
    tc_event ev = take_one(t);
    EXPECT_EQ(ev.u.key.key, TC_KEY_ESCAPE);
    ev = take_one(t);
    EXPECT_EQ(ev.u.key.key, TC_KEY_UP);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Parser: UTF-8 (docs/04 §7)
 * ------------------------------------------------------------------------ */

TEST(Parser, Utf8TwoByte) {
    tc_term_t* t = make_term();

    /* U+00E9 é = C3 A9. */
    tc_event ev = parse_one(t, "\xc3\xa9");
    EXPECT_EQ(ev.u.key.codepoint, 0x00E9u);
    EXPECT_EQ(ev.u.key.mods, 0);

    tc_term_destroy(t);
}

TEST(Parser, Utf8ThreeByte) {
    tc_term_t* t = make_term();

    /* U+4E2D 中 = E4 B8 AD. */
    tc_event ev = parse_one(t, "\xe4\xb8\xad");
    EXPECT_EQ(ev.u.key.codepoint, 0x4E2Du);

    tc_term_destroy(t);
}

TEST(Parser, Utf8SplitAcrossBatches) {
    tc_term_t* t = make_term();

    /* The 3-byte sequence arrives in two injections: parser must keep state. */
    EXPECT_EQ(tc_term_inject_input(t, "\xe4\xb8", 2), TC_OK);
    tc_event ev;
    bool     has = false;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);   /* incomplete: no event yet */

    EXPECT_EQ(tc_term_inject_input(t, "\xad", 1), TC_OK);
    ev = take_one(t);
    EXPECT_EQ(ev.u.key.codepoint, 0x4E2Du);

    tc_term_destroy(t);
}

TEST(Parser, Utf8InvalidHeadYieldsReplacement) {
    tc_term_t* t = make_term();

    /* 0x80 is a lone continuation byte. */
    tc_event ev = parse_one(t, "\x80");
    EXPECT_EQ(ev.u.key.codepoint, 0xFFFDu);

    /* 0xC0 is an overlong head. */
    ev = parse_one(t, "\xc0\xaf");
    EXPECT_EQ(ev.u.key.codepoint, 0xFFFDu);

    tc_term_destroy(t);
}

TEST(Parser, Utf8InterruptedByAsciiYieldsReplacementThenChar) {
    tc_term_t* t = make_term();

    /* E4 B8 followed by 'x' (non-continuation): FFFD then 'x'. */
    EXPECT_EQ(tc_term_inject_input(t, "\xe4\xb8x", 3), TC_OK);
    tc_event ev = take_one(t);
    EXPECT_EQ(ev.u.key.codepoint, 0xFFFDu);
    ev = take_one(t);
    EXPECT_EQ(ev.u.key.codepoint, 'x');

    tc_term_destroy(t);
}

TEST(Parser, AltUtf8Character) {
    tc_term_t* t = make_term();

    /* ESC + é = Alt+é. */
    tc_event ev = parse_one(t, "\x1b\xc3\xa9");
    EXPECT_EQ(ev.u.key.codepoint, 0x00E9u);
    EXPECT_EQ(ev.u.key.mods, TC_MOD_ALT);

    tc_term_destroy(t);
}

TEST(Parser, AsciiModeDoesNotDecodeUtf8) {
    tc_term_options opt = input_options();
    opt.text_mode       = TC_TEXT_ASCII;
    tc_term_t* t        = make_term(opt);

    tc_event ev = parse_one(t, "\xc3\xa9");
    EXPECT_EQ(ev.u.key.codepoint, 0xFFFDu);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Parser: focus and bracketed paste (docs/04 §10)
 * ------------------------------------------------------------------------ */

TEST(Parser, FocusEventsGatedByFeature) {
    tc_term_t* t = make_term();   /* focus_events defaults to false */

    /* Recognized but not produced when the feature is off. */
    EXPECT_EQ(tc_term_inject_input(t, "\x1b[I", 3), TC_OK);
    tc_event ev;
    bool     has = true;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);

    tc_term_destroy(t);
}

TEST(Parser, FocusEventsEmittedWhenEnabled) {
    tc_term_options opt = input_options();
    opt.focus_events     = true;
    tc_term_t* t         = make_term(opt);

    tc_event ev = parse_one(t, "\x1b[I");
    EXPECT_EQ(ev.kind, TC_EV_FOCUS);
    EXPECT_TRUE(ev.u.focus.focused);

    ev = parse_one(t, "\x1b[O");
    EXPECT_EQ(ev.kind, TC_EV_FOCUS);
    EXPECT_FALSE(ev.u.focus.focused);

    tc_term_destroy(t);
}

TEST(Parser, BracketedPasteRoundTrip) {
    tc_term_t* t = make_term();   /* bracketed_paste defaults to true */

    /* Start paste, payload (including control chars kept verbatim), end. */
    EXPECT_EQ(tc_term_inject_input(t, "\x1b[200~ab\n\tcd\x1b[201~", 18), TC_OK);
    tc_event ev = take_one(t);
    EXPECT_EQ(ev.kind, TC_EV_PASTE);
    EXPECT_EQ(ev.u.paste.len, 6u);   /* "ab\n\tcd" */
    EXPECT_EQ(std::string(ev.u.paste.text, ev.u.paste.len), "ab\n\tcd");

    tc_term_destroy(t);
}

TEST(Parser, PasteSequenceSplitAcrossBatches) {
    tc_term_t* t = make_term();

    EXPECT_EQ(tc_term_inject_input(t, "\x1b[200~hello", 11), TC_OK);
    tc_event ev;
    bool     has = true;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);   /* not terminated yet */

    EXPECT_EQ(tc_term_inject_input(t, "\x1b[20", 4), TC_OK);
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);

    EXPECT_EQ(tc_term_inject_input(t, "1~", 2), TC_OK);
    ev = take_one(t);
    EXPECT_EQ(ev.kind, TC_EV_PASTE);
    EXPECT_EQ(std::string(ev.u.paste.text, ev.u.paste.len), "hello");

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Parser: mouse (docs/04 §9)
 * ------------------------------------------------------------------------ */

TEST(Parser, MouseSgrClickGatedOffByDefault) {
    tc_term_t* t = make_term();   /* mouse_mode defaults to off */

    EXPECT_EQ(tc_term_inject_input(t, "\x1b[<0;5;5M", 9), TC_OK);
    tc_event ev;
    bool     has = true;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);   /* recognized but suppressed */

    tc_term_destroy(t);
}

TEST(Parser, MouseSgrClickAndRelease) {
    tc_term_options opt = input_options();
    opt.mouse_mode       = TC_MOUSE_CLICK;
    tc_term_t* t         = make_term(opt);

    /* SGR left-button press at 1-based (5,5) -> 0-based (4,4). */
    tc_event ev = parse_one(t, "\x1b[<0;5;5M");
    EXPECT_EQ(ev.kind, TC_EV_MOUSE);
    EXPECT_EQ(ev.u.mouse.x, 4);
    EXPECT_EQ(ev.u.mouse.y, 4);
    EXPECT_EQ(ev.u.mouse.button, TC_MOUSE_LEFT);
    EXPECT_EQ(ev.u.mouse.action, TC_MOUSE_ACT_DOWN);
    EXPECT_EQ(ev.u.mouse.mods, 0);

    ev = parse_one(t, "\x1b[<0;5;5m");
    EXPECT_EQ(ev.u.mouse.action, TC_MOUSE_ACT_UP);
    EXPECT_EQ(ev.u.mouse.button, TC_MOUSE_LEFT);

    tc_term_destroy(t);
}

TEST(Parser, MouseSgrModifiersAndWheel) {
    tc_term_options opt = input_options();
    opt.mouse_mode       = TC_MOUSE_CLICK;
    tc_term_t* t         = make_term(opt);

    /* Shift+Ctrl (4 + 16 = 20) press. */
    tc_event ev = parse_one(t, "\x1b[<20;1;1M");
    EXPECT_EQ(ev.u.mouse.mods, TC_MOD_SHIFT | TC_MOD_CTRL);
    EXPECT_EQ(ev.u.mouse.action, TC_MOUSE_ACT_DOWN);

    /* Wheel up (64). */
    ev = parse_one(t, "\x1b[<64;2;3M");
    EXPECT_EQ(ev.u.mouse.action, TC_MOUSE_ACT_WHEEL_UP);
    EXPECT_EQ(ev.u.mouse.button, TC_MOUSE_WHEEL);

    /* Wheel down (65). */
    ev = parse_one(t, "\x1b[<65;2;3M");
    EXPECT_EQ(ev.u.mouse.action, TC_MOUSE_ACT_WHEEL_DOWN);

    tc_term_destroy(t);
}

TEST(Parser, MouseSgrDragRequiresDragMode) {
    tc_term_options opt = input_options();
    opt.mouse_mode       = TC_MOUSE_DRAG;
    tc_term_t* t         = make_term(opt);

    /* Press, then motion while held (32 + 0 = motion with left held). */
    (void)parse_one(t, "\x1b[<0;1;1M");
    tc_event ev = parse_one(t, "\x1b[<32;2;2M");
    EXPECT_EQ(ev.u.mouse.action, TC_MOUSE_ACT_DRAG);
    EXPECT_EQ(ev.u.mouse.button, TC_MOUSE_LEFT);

    /* Release (0 m) ends the drag. */
    ev = parse_one(t, "\x1b[<0;2;2m");
    EXPECT_EQ(ev.u.mouse.action, TC_MOUSE_ACT_UP);

    /* Motion without a held button -> MOVE, still allowed in DRAG mode? No:
     * DRAG mode gates MOVE out, so nothing is produced. */
    EXPECT_EQ(tc_term_inject_input(t, "\x1b[<32;3;3M", 9), TC_OK);
    bool has = true;
    tc_event e2;
    EXPECT_EQ(tc_get_event(t, &e2, &has), TC_OK);
    EXPECT_FALSE(has);

    tc_term_destroy(t);
}

TEST(Parser, MouseX10ThreeBytePayload) {
    tc_term_options opt = input_options();
    opt.mouse_mode       = TC_MOUSE_CLICK;
    tc_term_t* t         = make_term(opt);

    /* X10: ESC [ M Cb Cx Cy with 0x20 bias; left press at (1,1). */
    tc_event ev = parse_one(t, "\x1b[M !!");
    EXPECT_EQ(ev.kind, TC_EV_MOUSE);
    EXPECT_EQ(ev.u.mouse.x, 0);
    EXPECT_EQ(ev.u.mouse.y, 0);
    EXPECT_EQ(ev.u.mouse.button, TC_MOUSE_LEFT);
    EXPECT_EQ(ev.u.mouse.action, TC_MOUSE_ACT_DOWN);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Parser: robustness and cross-sequence behavior (docs/04 §6)
 * ------------------------------------------------------------------------ */

TEST(Parser, OversizedCsiSequenceIsDropped) {
    tc_term_t* t = make_term();

    /* 300 '1's then 'A': exceeds the 256-byte cap, so no event is emitted. */
    std::string big(300, '1');
    std::string seq = "\x1b[" + big + "A";
    EXPECT_EQ(tc_term_inject_input(t, seq.data(), seq.size()), TC_OK);
    tc_event ev;
    bool     has = true;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);

    tc_term_destroy(t);
}

TEST(Parser, TooManyCsiParamsDropped) {
    tc_term_t* t = make_term();

    /* 20 params: over the 16-param cap, sequence dropped. */
    std::string seq = "\x1b[";
    for (int i = 0; i < 20; i++) seq += i ? ";1" : "1";
    seq += "A";
    EXPECT_EQ(tc_term_inject_input(t, seq.data(), seq.size()), TC_OK);
    tc_event ev;
    bool     has = true;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);

    tc_term_destroy(t);
}

TEST(Parser, UnknownSequenceDroppedWithoutSpillingBytes) {
    tc_term_t* t = make_term();

    /* "ESC [ q" is not a known input sequence: fully dropped, no events. */
    EXPECT_EQ(tc_term_inject_input(t, "\x1b[q", 3), TC_OK);
    tc_event ev;
    bool     has = true;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);

    /* A following printable key still works. */
    ev = parse_one(t, "x");
    EXPECT_EQ(ev.u.key.codepoint, 'x');

    tc_term_destroy(t);
}

TEST(Parser, C0InsideCsiAbortsSequenceAndReprocesses) {
    tc_term_t* t = make_term();

    /* "ESC [ " then CR: the CSI is aborted, CR becomes Enter. */
    EXPECT_EQ(tc_term_inject_input(t, "\x1b[\r", 3), TC_OK);
    tc_event ev = take_one(t);
    EXPECT_EQ(ev.u.key.key, TC_KEY_ENTER);

    tc_term_destroy(t);
}

TEST(Parser, MultipleKeysInOneBatchOrdered) {
    tc_term_t* t = make_term();

    EXPECT_EQ(tc_term_inject_input(t, "ab\x1b[C", 5), TC_OK);
    tc_event ev = take_one(t);
    EXPECT_EQ(ev.u.key.codepoint, 'a');
    ev = take_one(t);
    EXPECT_EQ(ev.u.key.codepoint, 'b');
    ev = take_one(t);
    EXPECT_EQ(ev.u.key.key, TC_KEY_RIGHT);

    tc_term_destroy(t);
}

TEST(Parser, KittyProtocolBasic) {
    tc_term_t* t = make_term();

    /* Kitty: CSI 97 ; 4 u = Ctrl+a (bit-set: 4 = Ctrl). */
    tc_event ev = parse_one(t, "\x1b[97;4u");
    EXPECT_EQ(ev.u.key.codepoint, 'a');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_CTRL);

    /* Shift (1). */
    ev = parse_one(t, "\x1b[97;1u");
    EXPECT_EQ(ev.u.key.mods, TC_MOD_SHIFT);

    /* Release event: CSI 97 ; 4 ; 3 u. */
    ev = parse_one(t, "\x1b[97;4;3u");
    EXPECT_EQ(ev.u.key.codepoint, 'a');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_CTRL);
    EXPECT_TRUE(ev.u.key.is_release);

    tc_term_destroy(t);
}

TEST(Parser, ModifyOtherKeysForm) {
    tc_term_t* t = make_term();

    /* CSI 27 ; 5 ; 98 ~ = Ctrl+b (5 = Ctrl). */
    tc_event ev = parse_one(t, "\x1b[27;5;98~");
    EXPECT_EQ(ev.u.key.codepoint, 'b');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_CTRL);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * ESC ambiguity settlement (docs/04 §11)
 * ------------------------------------------------------------------------ */

TEST(ParseSettle, FlushPendingSettlesLoneEsc) {
    tc_term_t* t = make_term();

    EXPECT_EQ(tc_term_inject_input(t, "\x1b", 1), TC_OK);
    /* Pull once so the injected bytes reach the parser and the ESC becomes a
     * residual (it is not settled on the normal fill path before the
     * deadline). */
    tc_event tmp;
    bool     has = false;
    EXPECT_EQ(tc_get_event(t, &tmp, &has), TC_OK);
    EXPECT_FALSE(has);

    EXPECT_EQ(tc_term_flush_pending(t, &has), TC_OK);
    EXPECT_TRUE(has);

    tc_event ev = take_one(t);
    EXPECT_EQ(ev.u.key.key, TC_KEY_ESCAPE);

    tc_term_destroy(t);
}

TEST(ParseSettle, FlushPendingDiscardsPartialSequence) {
    tc_term_t* t = make_term();

    /* A partial CSI is discarded by flush, not turned into a key. */
    EXPECT_EQ(tc_term_inject_input(t, "\x1b[1;", 4), TC_OK);
    tc_event tmp;
    bool     has = false;
    EXPECT_EQ(tc_get_event(t, &tmp, &has), TC_OK);
    EXPECT_FALSE(has);   /* parsed but not settled yet */

    has = true;
    EXPECT_EQ(tc_term_flush_pending(t, &has), TC_OK);
    EXPECT_FALSE(has);

    tc_term_destroy(t);
}

TEST(ParseSettle, PendingEscCompletesWithNextBatch) {
    tc_term_t* t = make_term();

    EXPECT_EQ(tc_term_inject_input(t, "\x1b", 1), TC_OK);
    /* New bytes arrive before the deadline: ESC splices into Alt+a. */
    EXPECT_EQ(tc_term_inject_input(t, "a", 1), TC_OK);
    tc_event ev = take_one(t);
    EXPECT_EQ(ev.u.key.codepoint, 'a');
    EXPECT_EQ(ev.u.key.mods, TC_MOD_ALT);

    tc_term_destroy(t);
}

/* --------------------------------------------------------------------------
 * Resize events (docs/04 §12)
 * ------------------------------------------------------------------------ */

TEST(ParseSettle, SetSizeProducesResizeEvent) {
    tc_term_t* t = make_term();

    EXPECT_EQ(tc_term_set_size(t, 120, 40), TC_OK);
    tc_event ev = take_one(t);
    EXPECT_EQ(ev.kind, TC_EV_RESIZE);
    EXPECT_EQ(ev.u.resize.cols, 120);
    EXPECT_EQ(ev.u.resize.rows, 40);

    tc_term_destroy(t);
}

TEST(ParseSettle, SameSizeDoesNotRepeatResizeEvent) {
    tc_term_t* t = make_term();

    EXPECT_EQ(tc_term_set_size(t, 100, 30), TC_OK);
    tc_event ev = take_one(t);
    EXPECT_EQ(ev.kind, TC_EV_RESIZE);

    /* Same size again: coalesced, no second event. */
    EXPECT_EQ(tc_term_set_size(t, 100, 30), TC_OK);
    bool has = true;
    EXPECT_EQ(tc_get_event(t, &ev, &has), TC_OK);
    EXPECT_FALSE(has);

    tc_term_destroy(t);
}

