#ifndef TERMCORE_TC_PLATFORM_H
#define TERMCORE_TC_PLATFORM_H

#include <stdbool.h>
#include <termcore/tc_export.h>

/* See docs/08-platform-notes.md. The platform layer is the only place that
 * knows about OS APIs; everything above it sees capabilities and events. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum tc_platform {
    TC_PLATFORM_UNKNOWN = 0,
    TC_PLATFORM_WINDOWS,
    TC_PLATFORM_LINUX,
    TC_PLATFORM_MACOS,
    TC_PLATFORM_BSD
} tc_platform;

typedef enum tc_backend_kind {
    TC_BACKEND_NULL = 0,   /* headless: no terminal handle at all */
    TC_BACKEND_WIN32,      /* Windows Console / ConPTY */
    TC_BACKEND_POSIX       /* termios + poll/select */
} tc_backend_kind;

TC_API tc_platform     tc_platform_current(void);
TC_API const char*     tc_platform_name(tc_platform p);
TC_API tc_backend_kind tc_platform_default_backend(void);

/* True when stdout is a terminal. Used to decide whether DA queries and raw
 * mode make sense at all (docs/03 §2, docs/09 §2). */
TC_API bool tc_stdout_is_tty(void);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_TC_PLATFORM_H */
