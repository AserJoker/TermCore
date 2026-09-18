#include "icu_data.h"
#include "unicode/udata.h"
#include "unicode/utypes.h"
#include "unicode/putil.h"   /* u_setDataDirectory */

/* External-data bootstrap: point ICU at the directory that holds icudt*.dat.
 * The .dat is staged next to the build tree (${CMAKE_BINARY_DIR}/data) by
 * third_party/icu/CMakeLists.txt; it is NOT embedded in the binary. */
int icu_data_init(const char* data_dir) {
  UErrorCode status = U_ZERO_ERROR;

  if (data_dir && *data_dir) {
    u_setDataDirectory(data_dir);
  }

  /* Force the common data to load so a missing/mismatched icudt*.dat fails
   * here, at startup, rather than on the first Unicode query. Opening the
   * "cnvalias" (converter aliases) package touches real data and succeeds for
   * the external icudt*.dat. */
  {
    UDataMemory* m = udata_open(NULL, "icu", "cnvalias", &status);
    if (m) {
      udata_close(m);
    }
  }

  return U_FAILURE(status) ? -1 : 0;
}
