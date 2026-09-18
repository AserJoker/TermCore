#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <termcore/tc_text.h>

#include "icu_bootstrap.h"

namespace {

/* Number of grapheme clusters in a UTF-8 string. */
int cluster_count(const char* s, size_t len) {
    const char* p = s;
    const char* end = s + len;
    int n = 0;
    tc_grapheme g;
    while (tc_text_grapheme_next(&p, end, &g)) n++;
    EXPECT_EQ(p, end);   /* full consumption */
    return n;
}

/* First cluster of a UTF-8 string (bytes + first codepoint). */
tc_grapheme first_cluster(const char* s, size_t len) {
    const char* p = s;
    const char* end = s + len;
    tc_grapheme g;
    memset(&g, 0, sizeof(g));
    bool ok = tc_text_grapheme_next(&p, end, &g);
    EXPECT_TRUE(ok);
    return g;
}

}  // namespace

TEST(Grapheme, EmptyAndNull) {
    tc_grapheme g;
    const char* p = "";
    EXPECT_FALSE(tc_text_grapheme_next(&p, p, &g));

    const char* q = "x";
    EXPECT_FALSE(tc_text_grapheme_next(nullptr, q + 1, &g));
    EXPECT_FALSE(tc_text_grapheme_next(&q, nullptr, &g));
    EXPECT_FALSE(tc_text_grapheme_next(&q, q + 1, nullptr));

    size_t off = 0;
    EXPECT_FALSE(tc_text_grapheme_next_off("", 0, &off, &g));
}

TEST(Grapheme, AsciiOneByteOneCluster) {
    const char* s = "abc";
    EXPECT_EQ(cluster_count(s, 3), 3);

    tc_grapheme g = first_cluster(s, 3);
    EXPECT_EQ(g.first_cp, 0x61u);
    EXPECT_EQ(g.end - g.begin, 1);
}

TEST(Grapheme, CjkEachIsOwnCluster) {
    const char* s = "\xE4\xBD\xA0\xE5\xA5\xBD";   /* 你好 */
    EXPECT_EQ(cluster_count(s, 6), 2);

    tc_grapheme g = first_cluster(s, 6);
    EXPECT_EQ(g.first_cp, 0x4F60u);
    EXPECT_EQ(g.end - g.begin, 3);
}

TEST(Grapheme, CombiningMarkJoinsBase) {
    /* a + U+0301 combining acute = one cluster. */
    TC_REQUIRE_ICU();
    const char* s = "a\xCC\x81";
    EXPECT_EQ(cluster_count(s, 3), 1);

    tc_grapheme g = first_cluster(s, 3);
    EXPECT_EQ(g.first_cp, 0x61u);
    EXPECT_EQ(g.end - g.begin, 3);
}

TEST(Grapheme, CrLfIsOneCluster) {
    TC_REQUIRE_ICU();
    const char* s = "\r\n";
    EXPECT_EQ(cluster_count(s, 2), 1);

    tc_grapheme g = first_cluster(s, 2);
    EXPECT_EQ(g.first_cp, 0x0Du);
    EXPECT_EQ(g.end - g.begin, 2);
}

TEST(Grapheme, ZwijEmojiFamilyIsOneCluster) {
    /* 👨 ZWJ 👩 ZWJ 👧 = one grapheme (GB11). */
    TC_REQUIRE_ICU();
    const char* s = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"
                    "\xE2\x80\x8D\xF0\x9F\x91\xA7";
    EXPECT_EQ(cluster_count(s, 18), 1);
}

TEST(Grapheme, RegionalIndicatorsPairUp) {
    /* 🇨 🇳 = one cluster (flag); third RI starts a new cluster (GB12/13). */
    TC_REQUIRE_ICU();
    const char* pair = "\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3";   /* 2 RIs */
    const char* triple = "\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3"
                         "\xF0\x9F\x87\xBA";                 /* 3 RIs */

    EXPECT_EQ(cluster_count(pair, 8), 1);
    EXPECT_EQ(cluster_count(triple, 12), 2);
}

TEST(Grapheme, HangulCombines) {
    /* ᄒ + ᅡ + ᆫ (Hangul Jamo) = one syllable cluster. */
    TC_REQUIRE_ICU();
    const char* s = "\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB";
    EXPECT_EQ(cluster_count(s, 9), 1);
}

TEST(Grapheme, NextOffMatchesNext) {
    const char* s = "a\xCC\x81\xE4\xBD\xA0""b";   /* a+comb, 你, b */
    TC_REQUIRE_ICU();
    size_t len = strlen(s);

    /* Offset-based iteration. */
    size_t off = 0;
    int n = 0;
    tc_grapheme g;
    while (tc_text_grapheme_next_off(s, len, &off, &g)) n++;
    EXPECT_EQ(n, 3);
    EXPECT_EQ(off, len);

    /* First cluster boundaries agree with the pointer-based API. */
    tc_grapheme g1 = first_cluster(s, len);
    off = 0;
    tc_grapheme g2;
    EXPECT_TRUE(tc_text_grapheme_next_off(s, len, &off, &g2));
    EXPECT_EQ(g1.begin - s, 0);
    EXPECT_EQ(g1.end - s, g2.end - g2.begin);
    EXPECT_EQ(g1.first_cp, g2.first_cp);
}

TEST(Grapheme, InvalidBytesYieldReplacementClusters) {
    const char* s = "\xFF\xE4\xBD";   /* lone 0xFF + truncated 你 */
    int n = cluster_count(s, 3);
    /* 0xFF -> one invalid cluster (U+FFFD); \xE4\xBD truncated at stream end
     * settles as another U+FFFD cluster. Exactly two clusters, fully consumed. */
    EXPECT_EQ(n, 2);
}
