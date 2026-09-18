#ifndef TERMCORE_THIRD_PARTY_ICU_DATA_H
#define TERMCORE_THIRD_PARTY_ICU_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the ICU common data directory (the directory containing icudt*.dat,
 * e.g. ${CMAKE_BINARY_DIR}/data in the CMake build).
 *
 * Must be called once at program startup, before any ICU API is used.
 * Passing NULL (or "") falls back to the ICU_DATA environment variable and
 * the ICU library's own compiled-in default.
 *
 * Returns 0 on success, -1 on failure. */
int icu_data_init(const char* data_dir);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_THIRD_PARTY_ICU_DATA_H */
