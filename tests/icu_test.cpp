#include <gtest/gtest.h>

#ifdef TERMCORE_USE_ICU

#include <cstdint>

#include <termcore/tc.h>
#include <unicode/uchar.h>     /* u_getIntPropertyValue */
#include <unicode/ubrk.h>      /* UBreakIterator, UBRK_CHARACTER */
#include <unicode/umachine.h>
#include <unicode/urename.h>

#include "icu_data.h"

namespace {

TEST(Icu, DataLoadsAndWidthsAreAvailable) {
    /* Point ICU at the externally-staged icudt*.dat. */
    ASSERT_EQ(icu_data_init(TC_ICU_DATA_DIR), 0)
        << "data dir: " TC_ICU_DATA_DIR;

    /* East Asian Width via UAX#11 data: U+4E00 (一) is Wide (2 columns),
     * U+0061 (a) is Narrow (1 column). These come from real UCD data, proving
     * the external data package is loaded and usable. */
    EXPECT_EQ(u_getIntPropertyValue(0x4E00, UCHAR_EAST_ASIAN_WIDTH),
              U_EA_WIDE);
    EXPECT_EQ(u_getIntPropertyValue(0x0061, UCHAR_EAST_ASIAN_WIDTH),
              U_EA_NARROW);
    /* U+FF21 (Ａ fullwidth A) is Fullwidth (2 columns). */
    EXPECT_EQ(u_getIntPropertyValue(0xFF21, UCHAR_EAST_ASIAN_WIDTH),
              U_EA_FULLWIDTH);
}

TEST(Icu, GraphemeBreakIterationWorks) {
    /* gtest_discover_tests runs each TEST in its own process, so every test
     * that touches ICU must load the external data itself. */
    ASSERT_EQ(icu_data_init(TC_ICU_DATA_DIR), 0);

    /* UAX#29: a base + combining mark forms one grapheme cluster
     * (a + U+0301 combining acute). */
    UErrorCode status = U_ZERO_ERROR;
    /* Empty locale: CHARACTER break rules need no locale-specific resources. */
    UBreakIterator* bi = ubrk_open(UBRK_CHARACTER, "", nullptr, 0, &status);
    ASSERT_TRUE(U_SUCCESS(status)) << u_errorName(status);
    ASSERT_NE(bi, nullptr);

    const UChar text[] = {0x0061, 0x0301, 0x0062};   /* "áb" */
    ubrk_setText(bi, text, 3, &status);
    ASSERT_TRUE(U_SUCCESS(status));

    int32_t b0 = ubrk_first(bi);
    int32_t b1 = ubrk_next(bi);
    int32_t b2 = ubrk_next(bi);
    int32_t b3 = ubrk_next(bi);
    EXPECT_EQ(b0, 0);
    EXPECT_EQ(b1, 2);   /* a + combining accent stay together */
    EXPECT_EQ(b2, 3);
    EXPECT_EQ(b3, UBRK_DONE);

    ubrk_close(bi);
}

}  // namespace

#else  /* !TERMCORE_USE_ICU */

namespace {

TEST(Icu, DisabledIsSkipped) {
    GTEST_SKIP() << "TERMCORE_USE_ICU is OFF; ICU backend not built";
}

}  // namespace

#endif  /* TERMCORE_USE_ICU */
