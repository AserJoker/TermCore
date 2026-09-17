#ifndef TERMCORE_TC_STATUS_H
#define TERMCORE_TC_STATUS_H

#include <termcore/tc_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* See docs/07-memory-and-errors.md §2 for classification and handling rules. */
typedef enum tc_status {
    TC_OK = 0,

    /* generic */
    TC_ERR_FAIL          = 1,   /* unclassified failure */
    TC_ERR_INVALID_ARG   = 2,   /* NULL / out of range / illegal combination */
    TC_ERR_STATE         = 3,   /* operation not allowed in current state */
    TC_ERR_NOMEM         = 4,   /* allocation failed */
    TC_ERR_UNSUPPORTED   = 5,   /* platform / backend / capability does not support it */
    TC_ERR_NOT_FOUND     = 6,   /* handle or id does not exist */
    TC_ERR_VERSION       = 7,   /* format / ABI / profile version mismatch */

    /* I/O and backend */
    TC_ERR_IO            = 10,  /* read / write failure */
    TC_ERR_NOT_A_TTY     = 11,  /* not a terminal (and headless not enabled) */
    TC_ERR_PERM          = 12,  /* permission denied */
    TC_ERR_BACKEND       = 13,  /* backend init or call failure */

    /* time and capacity */
    TC_ERR_TIMEOUT       = 20,  /* wait timed out (often non-fatal) */
    TC_ERR_OVERFLOW      = 21,  /* buffer too small / queue full */
    TC_ERR_TRUNCATED     = 22,  /* data truncated, partially processed */

    /* parsing and text */
    TC_ERR_PARSE         = 30,  /* escape sequence parse failure (recovered) */
    TC_ERR_ENCODING      = 31,  /* invalid encoding (replacement applied) */

    /* pluggable components */
    TC_ERR_SINK          = 40,  /* one or more sink callbacks failed */
    TC_ERR_SOURCE        = 41,  /* external input source failed */
} tc_status;

/* Stable English name, e.g. "TC_ERR_NOMEM"; unknown codes map to "TC_ERR_FAIL". */
TC_API const char* tc_status_string(tc_status s);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_TC_STATUS_H */
