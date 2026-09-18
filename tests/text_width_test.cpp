#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include <termcore/tc_text.h>

#include "icu_bootstrap.h"

namespace {

constexpr bool kAmbiguousWide = true;
constexpr bool kAmbiguousNarrow = false;

}  // namespace

/* --------------------------------------------------------------------------
 * tc_text_char_width
 * ------------------------------------------------------------------------ */

TEST(CharWidth, AsciiModeAlwaysOne) {
    EXPECT_EQ(tc_text_char_width(0x41, TC_TEXT_ASCII, false), 1);
    EXPECT_EQ(tc_text_char_width(0x4E00, TC_TEXT_ASCII, false), 1);
    EXPECT_EQ(tc_text_char_width(0x0301, TC_TEXT_ASCII, false), 1);
    EXPECT_EQ(tc_text_char_width(0x0000, TC_TEXT_ASCII, false), 1);
}

TEST(CharWidth, ControlCharsAreZero) {
    EXPECT_EQ(tc_text_char_width(0x0000, TC_TEXT_UNICODE, false), 0);
    EXPECT_EQ(tc_text_char_width(0x0009, TC_TEXT_UNICODE, false), 0);   /* TAB */
    EXPECT_EQ(tc_text_char_width(0x001B, TC_TEXT_UNICODE, false), 0);   /* ESC */
    EXPECT_EQ(tc_text_char_width(0x007F, TC_TEXT_UNICODE, false), 0);
    EXPECT_EQ(tc_text_char_width(0x0080, TC_TEXT_UNICODE, false), 0);   /* C1 */
    EXPECT_EQ(tc_text_char_width(0x009F, TC_TEXT_UNICODE, false), 0);
}

TEST(CharWidth, AsciiWidthOne) {
    EXPECT_EQ(tc_text_char_width(0x20, TC_TEXT_UNICODE, false), 1);
    EXPECT_EQ(tc_text_char_width(0x41, TC_TEXT_UNICODE, false), 1);
    EXPECT_EQ(tc_text_char_width(0x7E, TC_TEXT_UNICODE, false), 1);
}

TEST(CharWidth, CjkIsWide) {
    EXPECT_EQ(tc_text_char_width(0x4E00, TC_TEXT_UNICODE, false), 2);   /* 一 */
    EXPECT_EQ(tc_text_char_width(0x4F60, TC_TEXT_UNICODE, false), 2);   /* 你 */
    EXPECT_EQ(tc_text_char_width(0x3042, TC_TEXT_UNICODE, false), 2);   /* あ */
    EXPECT_EQ(tc_text_char_width(0xFF21, TC_TEXT_UNICODE, false), 2);   /* Ａ */
}

TEST(CharWidth, CombiningMarksAreZero) {
    EXPECT_EQ(tc_text_char_width(0x0301, TC_TEXT_UNICODE, false), 0);   /* combining acute */
    EXPECT_EQ(tc_text_char_width(0xFE0F, TC_TEXT_UNICODE, false), 0);   /* VS16 */
    EXPECT_EQ(tc_text_char_width(0xFE0E, TC_TEXT_UNICODE, false), 0);   /* VS15 */
}

TEST(CharWidth, AmbiguousWidthRespectsFlag) {
    /* U+00A1 ¡ is East Asian Ambiguous. */
    EXPECT_EQ(tc_text_char_width(0x00A1, TC_TEXT_UNICODE, kAmbiguousNarrow), 1);
    EXPECT_EQ(tc_text_char_width(0x00A1, TC_TEXT_UNICODE, kAmbiguousWide), 2);
}

/* --------------------------------------------------------------------------
 * tc_text_grapheme_width
 * ------------------------------------------------------------------------ */

static tc_grapheme make_grapheme(const char* s) {
    const char* p = s;
    const char* end = s + strlen(s);
    tc_grapheme g;
    bool ok = tc_text_grapheme_next(&p, end, &g);
    EXPECT_TRUE(ok);
    return g;
}

TEST(GraphemeWidth, Basic) {
    tc_grapheme g = make_grapheme("a");
    EXPECT_EQ(tc_text_grapheme_width(&g, TC_TEXT_UNICODE, false), 1);

    g = make_grapheme("\xE4\xBD\xA0");   /* 你 */
    EXPECT_EQ(tc_text_grapheme_width(&g, TC_TEXT_UNICODE, false), 2);

    g = make_grapheme("\r\n");
    EXPECT_EQ(tc_text_grapheme_width(&g, TC_TEXT_UNICODE, false), 0);
}

TEST(GraphemeWidth, CombiningClusterKeepsBaseWidth) {
    /* a + combining acute: still width 1. */
    tc_grapheme g = make_grapheme("a\xCC\x81");
    EXPECT_EQ(tc_text_grapheme_width(&g, TC_TEXT_UNICODE, false), 1);

    /* CJK + combining mark: still width 2. */
    g = make_grapheme("\xE4\xBD\xA0\xCC\x81");
    EXPECT_EQ(tc_text_grapheme_width(&g, TC_TEXT_UNICODE, false), 2);
}

TEST(GraphemeWidth, EmojiZwjIsWide) {
    tc_grapheme g = make_grapheme("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9");
    EXPECT_EQ(tc_text_grapheme_width(&g, TC_TEXT_UNICODE, false), 2);
}

TEST(GraphemeWidth, FlagIsWide) {
    TC_REQUIRE_ICU();
    tc_grapheme g = make_grapheme("\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3");
    EXPECT_EQ(tc_text_grapheme_width(&g, TC_TEXT_UNICODE, false), 2);
}

TEST(GraphemeWidth, AsciiModeAlwaysOne) {
    tc_grapheme g = make_grapheme("\xE4\xBD\xA0");
    EXPECT_EQ(tc_text_grapheme_width(&g, TC_TEXT_ASCII, false), 1);
}

/* --------------------------------------------------------------------------
 * tc_text_string_width
 * ------------------------------------------------------------------------ */

TEST(StringWidth, Basic) {
    EXPECT_EQ(tc_text_string_width("", 0, TC_TEXT_UNICODE, false), 0);
    EXPECT_EQ(tc_text_string_width("abc", 3, TC_TEXT_UNICODE, false), 3);
    EXPECT_EQ(tc_text_string_width("\xE4\xBD\xA0\xE5\xA5\xBD", 6, TC_TEXT_UNICODE, false), 4);  /* 你好 */
    EXPECT_EQ(tc_text_string_width("a\xCC\x81", 3, TC_TEXT_UNICODE, false), 1);                 /* a+comb */
    EXPECT_EQ(tc_text_string_width("abc", 3, TC_TEXT_ASCII, false), 3);
    EXPECT_EQ(tc_text_string_width("\xE4\xBD\xA0", 3, TC_TEXT_ASCII, false), 3);                /* bytes, not cps */
}

TEST(StringWidth, AmbiguousAffectsTotal) {
    const char* s = "\xC2\xA1";   /* ¡ */
    EXPECT_EQ(tc_text_string_width(s, 2, TC_TEXT_UNICODE, kAmbiguousNarrow), 1);
    EXPECT_EQ(tc_text_string_width(s, 2, TC_TEXT_UNICODE, kAmbiguousWide), 2);
}
