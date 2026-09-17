#include <termcore/tc_status.h>

const char* tc_status_string(tc_status s) {
    switch (s) {
    case TC_OK:              return "TC_OK";
    case TC_ERR_FAIL:        return "TC_ERR_FAIL";
    case TC_ERR_INVALID_ARG: return "TC_ERR_INVALID_ARG";
    case TC_ERR_STATE:       return "TC_ERR_STATE";
    case TC_ERR_NOMEM:       return "TC_ERR_NOMEM";
    case TC_ERR_UNSUPPORTED: return "TC_ERR_UNSUPPORTED";
    case TC_ERR_NOT_FOUND:   return "TC_ERR_NOT_FOUND";
    case TC_ERR_VERSION:     return "TC_ERR_VERSION";
    case TC_ERR_IO:          return "TC_ERR_IO";
    case TC_ERR_NOT_A_TTY:   return "TC_ERR_NOT_A_TTY";
    case TC_ERR_PERM:        return "TC_ERR_PERM";
    case TC_ERR_BACKEND:     return "TC_ERR_BACKEND";
    case TC_ERR_TIMEOUT:     return "TC_ERR_TIMEOUT";
    case TC_ERR_OVERFLOW:    return "TC_ERR_OVERFLOW";
    case TC_ERR_TRUNCATED:   return "TC_ERR_TRUNCATED";
    case TC_ERR_PARSE:       return "TC_ERR_PARSE";
    case TC_ERR_ENCODING:    return "TC_ERR_ENCODING";
    case TC_ERR_SINK:        return "TC_ERR_SINK";
    case TC_ERR_SOURCE:      return "TC_ERR_SOURCE";
    default:                 return "TC_ERR_FAIL";
    }
}
