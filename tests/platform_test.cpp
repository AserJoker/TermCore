#include <gtest/gtest.h>

#include <termcore/tc.h>

TEST(Platform, CurrentIsKnownOnSupportedHosts) {
    const tc_platform p = tc_platform_current();
    EXPECT_NE(p, TC_PLATFORM_UNKNOWN);
}

TEST(Platform, NameIsNeverNull) {
    for (int i = 0; i <= static_cast<int>(TC_PLATFORM_BSD); ++i) {
        EXPECT_NE(tc_platform_name(static_cast<tc_platform>(i)), nullptr);
    }
}

TEST(Platform, DefaultBackendMatchesHost) {
#ifdef _WIN32
    EXPECT_EQ(tc_platform_default_backend(), TC_BACKEND_WIN32);
#else
    EXPECT_EQ(tc_platform_default_backend(), TC_BACKEND_POSIX);
#endif
}

TEST(Platform, IsTtyDoesNotCrash) {
    /* Under ctest stdout is a pipe; the call must simply answer. */
    (void)tc_stdout_is_tty();
}
