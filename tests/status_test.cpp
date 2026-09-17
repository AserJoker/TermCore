#include <gtest/gtest.h>

#include <cstring>

#include <termcore/tc.h>

TEST(Status, OkIsZero) {
    EXPECT_EQ(static_cast<int>(TC_OK), 0);
}

TEST(Status, EveryCodeHasAName) {
    for (int code = 0; code < 64; ++code) {
        const char* s = tc_status_string(static_cast<tc_status>(code));
        ASSERT_NE(s, nullptr);
        EXPECT_GT(std::strlen(s), 0u);
    }
}

TEST(Status, KnownCodesMapToThemselves) {
    EXPECT_STREQ(tc_status_string(TC_OK), "TC_OK");
    EXPECT_STREQ(tc_status_string(TC_ERR_NOMEM), "TC_ERR_NOMEM");
    EXPECT_STREQ(tc_status_string(TC_ERR_UNSUPPORTED), "TC_ERR_UNSUPPORTED");
    EXPECT_STREQ(tc_status_string(TC_ERR_VERSION), "TC_ERR_VERSION");
}

TEST(Status, UnknownCodesFallBackToFail) {
    EXPECT_STREQ(tc_status_string(static_cast<tc_status>(9999)), "TC_ERR_FAIL");
}
