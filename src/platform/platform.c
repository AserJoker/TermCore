#include <termcore/tc_platform.h>

#include <stdio.h>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

tc_platform tc_platform_current(void) {
#if defined(_WIN32)
    return TC_PLATFORM_WINDOWS;
#elif defined(__APPLE__)
    return TC_PLATFORM_MACOS;
#elif defined(__linux__)
    return TC_PLATFORM_LINUX;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(BSD)
    return TC_PLATFORM_BSD;
#else
    return TC_PLATFORM_UNKNOWN;
#endif
}

const char* tc_platform_name(tc_platform p) {
    switch (p) {
    case TC_PLATFORM_WINDOWS: return "windows";
    case TC_PLATFORM_LINUX:   return "linux";
    case TC_PLATFORM_MACOS:   return "macos";
    case TC_PLATFORM_BSD:     return "bsd";
    case TC_PLATFORM_UNKNOWN: /* fallthrough */
    default:                  return "unknown";
    }
}

tc_backend_kind tc_platform_default_backend(void) {
#if defined(_WIN32)
    return TC_BACKEND_WIN32;
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    return TC_BACKEND_POSIX;
#else
    return TC_BACKEND_NULL;
#endif
}

bool tc_stdout_is_tty(void) {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}
