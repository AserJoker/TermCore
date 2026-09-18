#ifndef TERMCORE_TESTS_ICU_BOOTSTRAP_H
#define TERMCORE_TESTS_ICU_BOOTSTRAP_H

/* gtest_discover_tests launches each TEST in its own process, so ICU data must
 * be loaded at process start. A file-scope initializer runs before main() and
 * points ICU at the externally-staged icudt*.dat (docs/05 §9.2). idempotent:
 * including this header from several test files is harmless. */

#include <gtest/gtest.h>

#ifdef TERMCORE_USE_ICU

#include "icu_data.h"

namespace termcore_test {

struct IcuBootstrap {
    IcuBootstrap() { icu_data_init(TC_ICU_DATA_DIR); }
};

[[maybe_unused]] static IcuBootstrap g_termcore_icu_bootstrap;

}  // namespace termcore_test

#endif  /* TERMCORE_USE_ICU */

/* Full UAX#29 cluster merging (GB3/GB6/7/8/GB9/GB11/GB12/13, emoji ZWJ, RI
 * pairing) is only provided by the ICU backend. Without it the degraded path
 * splits per codepoint (docs/05 §9.2). Tests that assert merged-cluster
 * semantics must require the ICU backend. */
#ifdef TERMCORE_USE_ICU
#define TC_REQUIRE_ICU()  do { /* ICU active */ } while (0)
#else
#define TC_REQUIRE_ICU()  GTEST_SKIP() << "cluster merging requires the ICU backend (TERMCORE_USE_ICU=ON)"
#endif

#endif  /* TERMCORE_TESTS_ICU_BOOTSTRAP_H */
