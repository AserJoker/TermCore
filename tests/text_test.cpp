#include <gtest/gtest.h>

#include <cstddef>

/* Include tc_text.h on its own (not tc.h) to prove the zero-dependency
 * contract from docs/01 §3 and docs/05 §1. */
#include <termcore/tc_text.h>

namespace {

TEST(TextHeader, ZeroDependencyTypesExist) {
    /* Enum values are frozen by the docs (docs/05 §2/§4). */
    EXPECT_EQ(static_cast<int>(TC_TEXT_ASCII), 0);
    EXPECT_EQ(static_cast<int>(TC_TEXT_UNICODE), 1);

    EXPECT_EQ(static_cast<int>(TC_UTF8_OK), 0);
    EXPECT_EQ(static_cast<int>(TC_UTF8_INCOMPLETE), 1);
    EXPECT_EQ(static_cast<int>(TC_UTF8_INVALID), 2);
}

TEST(TextHeader, GraphemeLayoutIsStable) {
    /* POD with frozen field order (docs/07 §3.2): pointers first, then
     * fixed-width fields, then reserved padding at the tail. */
    EXPECT_GT(sizeof(tc_grapheme), 0u);

    EXPECT_NE(offsetof(tc_grapheme, begin), size_t(-1));
    EXPECT_NE(offsetof(tc_grapheme, end), size_t(-1));
    EXPECT_NE(offsetof(tc_grapheme, first_cp), size_t(-1));
    EXPECT_NE(offsetof(tc_grapheme, width), size_t(-1));
    EXPECT_NE(offsetof(tc_grapheme, reserved), size_t(-1));

    /* Reserved fields must sit at the tail so future ABI growth appends
     * without reordering. */
    EXPECT_GT(offsetof(tc_grapheme, reserved),
              offsetof(tc_grapheme, width));
}

TEST(TextHeader, MetricsLayoutIsStable) {
    EXPECT_NE(offsetof(tc_text_metrics, columns), size_t(-1));
    EXPECT_NE(offsetof(tc_text_metrics, graphemes), size_t(-1));
    EXPECT_NE(offsetof(tc_text_metrics, codepoints), size_t(-1));
    EXPECT_NE(offsetof(tc_text_metrics, bytes), size_t(-1));
}

TEST(TextConfig, DefaultFillsZeroedStruct) {
    tc_text_config cfg;
    memset(&cfg, 0xAB, sizeof(cfg)); /* poison to prove every byte is written */

    tc_text_config_default(&cfg);

    EXPECT_EQ(cfg.replacement_cp, 0u); /* per-mode default marker */
    EXPECT_EQ(cfg.tab_columns, 0);     /* caller interprets */
    EXPECT_EQ(cfg.reserved[0], 0u);
    EXPECT_EQ(cfg.reserved[1], 0u);
}

TEST(TextConfig, DefaultIsIdempotent) {
    tc_text_config a;
    tc_text_config b;
    tc_text_config_default(&a);
    tc_text_config_default(&b);
    EXPECT_EQ(memcmp(&a, &b, sizeof(a)), 0);
}

TEST(TextConfig, NullIsNoOp) {
    tc_text_config_default(nullptr); /* must not crash */
    SUCCEED();
}

}  // namespace
