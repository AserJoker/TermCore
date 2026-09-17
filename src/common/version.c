#include <termcore/tc_version.h>

void tc_version_get(tc_version* out) {
    if (!out) return;
    out->major  = TERMCORE_VERSION_MAJOR;
    out->minor  = TERMCORE_VERSION_MINOR;
    out->patch  = TERMCORE_VERSION_PATCH;
    out->abi    = TERMCORE_ABI_VERSION;
    out->string = TERMCORE_VERSION_STRING;
}

const char* tc_version_string(void) {
    return TERMCORE_VERSION_STRING;
}

int32_t tc_version_abi(void) {
    return TERMCORE_ABI_VERSION;
}
