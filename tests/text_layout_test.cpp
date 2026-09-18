#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <termcore/tc_text.h>

#include "icu_bootstrap.h"

namespace {

/* 你 = U+4F60 (3 bytes, width 2); 好 = U+597D (3 bytes, width 2) */
constexpr bool kAmb = false;

}  // namespace

/* --------------------------------------------------------------------------
 * tc_text_measure
 * ------------------------------------------------------------------------ */

TEST(Measure, EmptyAndNull) {
    tc_text_metrics m = tc_text_measure("", 0, TC_TEXT_UNICODE, kAmb);
    EXPECT_EQ(m.columns, 0);
    EXPECT_EQ(m.graphemes, 0);
    EXPECT_EQ(m.codepoints, 0);
    EXPECT_EQ(m.bytes, 0);

    m = tc_text_measure(nullptr, 5, TC_TEXT_UNICODE, kAmb);
    EXPECT_EQ(m.columns, 0);
}

TEST(Measure, BasicCounts) {
    /* "a你b" -> columns 1+2+1=4, graphemes 3, codepoints 3, bytes 5 */
    const char* s = "a\xE4\xBD\xA0""b";
    tc_text_metrics m = tc_text_measure(s, 5, TC_TEXT_UNICODE, kAmb);
    EXPECT_EQ(m.columns, 4);
    EXPECT_EQ(m.graphemes, 3);
    EXPECT_EQ(m.codepoints, 3);
    EXPECT_EQ(m.bytes, 5);
}

TEST(Measure, CombiningMarkCountedInCluster) {
    /* "a" + U+0301: 1 grapheme, 2 codepoints, width 1 */
    TC_REQUIRE_ICU();
    const char* s = "a\xCC\x81";
    tc_text_metrics m = tc_text_measure(s, 3, TC_TEXT_UNICODE, kAmb);
    EXPECT_EQ(m.columns, 1);
    EXPECT_EQ(m.graphemes, 1);
    EXPECT_EQ(m.codepoints, 2);
    EXPECT_EQ(m.bytes, 3);
}

TEST(Measure, AsciiModeIsByteCounted) {
    const char* s = "\xE4\xBD\xA0";   /* 3 bytes */
    tc_text_metrics m = tc_text_measure(s, 3, TC_TEXT_ASCII, kAmb);
    EXPECT_EQ(m.columns, 3);
    EXPECT_EQ(m.graphemes, 3);
    EXPECT_EQ(m.codepoints, 3);
    EXPECT_EQ(m.bytes, 3);
}

TEST(Measure, InvalidBytesStillCounted) {
    /* \xFF is invalid: one U+FFFD cluster, width 1 */
    const char* s = "\xFF";
    tc_text_metrics m = tc_text_measure(s, 1, TC_TEXT_UNICODE, kAmb);
    EXPECT_EQ(m.columns, 1);
    EXPECT_EQ(m.graphemes, 1);
    EXPECT_EQ(m.codepoints, 1);
    EXPECT_EQ(m.bytes, 1);
}

/* --------------------------------------------------------------------------
 * tc_text_truncate_columns
 * ------------------------------------------------------------------------ */

TEST(Truncate, NullArgsFail) {
    size_t bytes = 0;
    EXPECT_FALSE(tc_text_truncate_columns(nullptr, 0, 5, TC_TEXT_UNICODE, kAmb,
                                          &bytes, nullptr, nullptr));
}

TEST(Truncate, NoTruncationWhenFits) {
    const char* s = "abc";
    size_t bytes = 0;
    int32_t cols = 0;
    bool truncated = true;
    EXPECT_TRUE(tc_text_truncate_columns(s, 3, 5, TC_TEXT_UNICODE, kAmb,
                                         &bytes, &cols, &truncated));
    EXPECT_EQ(bytes, 3u);
    EXPECT_EQ(cols, 3);
    EXPECT_FALSE(truncated);
}

TEST(Truncate, StopsAtWidthLimit) {
    /* "你好abc" width 2+2+1+1+1 = 7. max 4 -> 你好 */
    const char* s = "\xE4\xBD\xA0\xE5\xA5\xBD" "abc";
    size_t bytes = 0;
    int32_t cols = 0;
    bool truncated = false;
    EXPECT_TRUE(tc_text_truncate_columns(s, 9, 4, TC_TEXT_UNICODE, kAmb,
                                         &bytes, &cols, &truncated));
    EXPECT_EQ(bytes, 6u);     /* 你好 = 6 bytes */
    EXPECT_EQ(cols, 4);
    EXPECT_TRUE(truncated);
}

TEST(Truncate, DoesNotSplitCluster) {
    /* "ab你" width 1+1+2 = 4. max 3 -> the 你 (width 2) doesn't fit, so "ab". */
    const char* s = "ab\xE4\xBD\xA0";
    size_t bytes = 0;
    int32_t cols = 0;
    bool truncated = false;
    EXPECT_TRUE(tc_text_truncate_columns(s, 5, 3, TC_TEXT_UNICODE, kAmb,
                                         &bytes, &cols, &truncated));
    EXPECT_EQ(bytes, 2u);
    EXPECT_EQ(cols, 2);
    EXPECT_TRUE(truncated);
}

TEST(Truncate, ZeroMaxYieldsNothing) {
    const char* s = "abc";
    size_t bytes = 99;
    int32_t cols = 99;
    bool truncated = false;
    EXPECT_TRUE(tc_text_truncate_columns(s, 3, 0, TC_TEXT_UNICODE, kAmb,
                                         &bytes, &cols, &truncated));
    EXPECT_EQ(bytes, 0u);
    EXPECT_EQ(cols, 0);
    EXPECT_TRUE(truncated);
}

TEST(Truncate, AsciiModeTruncatesByByte) {
    const char* s = "\xE4\xBD\xA0";   /* 3 bytes */
    size_t bytes = 0;
    int32_t cols = 0;
    bool truncated = false;
    EXPECT_TRUE(tc_text_truncate_columns(s, 3, 2, TC_TEXT_ASCII, kAmb,
                                         &bytes, &cols, &truncated));
    EXPECT_EQ(bytes, 2u);
    EXPECT_EQ(cols, 2);
    EXPECT_TRUE(truncated);
}

/* --------------------------------------------------------------------------
 * tc_text_wrap_next
 * ------------------------------------------------------------------------ */

TEST(Wrap, NullAndEmpty) {
    size_t brk = 0;
    EXPECT_FALSE(tc_text_wrap_next(nullptr, 0, 10, TC_TEXT_UNICODE, kAmb, &brk));
    EXPECT_FALSE(tc_text_wrap_next("", 0, 10, TC_TEXT_UNICODE, kAmb, &brk));
    EXPECT_FALSE(tc_text_wrap_next("abc", 3, 10, TC_TEXT_UNICODE, kAmb, nullptr));
}

TEST(Wrap, FitsInOneLineReturnsFalse) {
    const char* s = "abc";
    size_t brk = 99;
    EXPECT_FALSE(tc_text_wrap_next(s, 3, 5, TC_TEXT_UNICODE, kAmb, &brk));
    EXPECT_EQ(brk, 3u);   /* whole string is the last line */
}

TEST(Wrap, BreaksAtSpace) {
    /* "hello world" width 11, max 6 -> break after "hello " (offset 6):
     * the space is a break opportunity that belongs to the current line. */
    const char* s = "hello world";
    size_t brk = 0;
    EXPECT_TRUE(tc_text_wrap_next(s, 11, 6, TC_TEXT_UNICODE, kAmb, &brk));
    EXPECT_EQ(brk, 6u);
}

TEST(Wrap, HardBreakWhenNoOpportunity) {
    /* "aaaaaa" no spaces, max 4 -> hard break at 4. */
    const char* s = "aaaaaa";
    size_t brk = 0;
    EXPECT_TRUE(tc_text_wrap_next(s, 6, 4, TC_TEXT_UNICODE, kAmb, &brk));
    EXPECT_EQ(brk, 4u);
}

TEST(Wrap, BreaksAfterCjk) {
    /* "你好你好" width 8, max 4 -> break after 你好 (offset 6). */
    const char* s = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xBD\xA0\xE5\xA5\xBD";
    size_t brk = 0;
    EXPECT_TRUE(tc_text_wrap_next(s, 12, 4, TC_TEXT_UNICODE, kAmb, &brk));
    EXPECT_EQ(brk, 6u);
}

TEST(Wrap, DoesNotSplitCjkClusterWithCombining) {
    /* "你" + combining mark + "好": width 2+2, max 3 -> break before 好. */
    TC_REQUIRE_ICU();
    const char* s = "\xE4\xBD\xA0\xCC\x81\xE5\xA5\xBD";
    size_t brk = 0;
    EXPECT_TRUE(tc_text_wrap_next(s, 8, 3, TC_TEXT_UNICODE, kAmb, &brk));
    /* Cluster "你"+comb is 5 bytes; break lands right after it. */
    EXPECT_EQ(brk, 5u);
}

TEST(Wrap, IterativeWrapping) {
    /* "hello world foo" width 15, max 6 -> ["hello ", "world ", "foo"] */
    const char* s = "hello world foo";
    size_t len = strlen(s);
    size_t off = 0;
    int lines = 0;
    size_t brk = 0;
    while (tc_text_wrap_next(s + off, len - off, 6, TC_TEXT_UNICODE, kAmb, &brk)) {
        lines++;
        off += brk;
    }
    EXPECT_EQ(lines, 2);
    EXPECT_EQ(off, 12u);   /* "hello world " */
}
