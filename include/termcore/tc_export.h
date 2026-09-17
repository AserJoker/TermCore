#ifndef TERMCORE_TC_EXPORT_H
#define TERMCORE_TC_EXPORT_H

/* Symbol visibility for the public API.
 *
 * Static builds        : TC_API expands to nothing.
 * Shared builds        : TERMCORE_BUILD_SHARED is defined while compiling the
 *                        library, TERMCORE_USE_SHARED while consuming it.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(TERMCORE_BUILD_SHARED)
#    define TC_API __declspec(dllexport)
#  elif defined(TERMCORE_USE_SHARED)
#    define TC_API __declspec(dllimport)
#  else
#    define TC_API
#  endif
#else
#  if defined(TERMCORE_BUILD_SHARED) && defined(__GNUC__)
#    define TC_API __attribute__((visibility("default")))
#  else
#    define TC_API
#  endif
#endif

#endif /* TERMCORE_TC_EXPORT_H */
