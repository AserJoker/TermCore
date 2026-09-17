#ifndef TERMCORE_TC_VERSION_H
#define TERMCORE_TC_VERSION_H

#include <stdint.h>
#include <termcore/tc_export.h>

#define TERMCORE_VERSION_MAJOR 0
#define TERMCORE_VERSION_MINOR 1
#define TERMCORE_VERSION_PATCH 0
#define TERMCORE_VERSION_STRING "0.1.0"

/* Bumped on any ABI-breaking change; cached data (e.g. capability profiles,
 * see docs/03 §10) records it and is discarded when it differs. */
#define TERMCORE_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tc_version {
    int32_t     major;
    int32_t     minor;
    int32_t     patch;
    int32_t     abi;
    const char* string;
} tc_version;

TC_API void        tc_version_get(tc_version* out);
TC_API const char* tc_version_string(void);
TC_API int32_t     tc_version_abi(void);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_TC_VERSION_H */
