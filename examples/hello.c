/* Minimal smoke example: version, platform probe, allocator round-trip.
 *
 * Build: cmake --preset windows-clang && cmake --build --preset windows-clang
 * Run:   build/windows-clang/bin/termcore_hello
 */
#include <stdio.h>

#include <termcore/tc.h>

int main(void) {
    tc_version v;
    tc_version_get(&v);

    printf("termcore %s (abi %d)\n", v.string, v.abi);
    printf("platform: %s\n", tc_platform_name(tc_platform_current()));
    printf("backend : %d\n", (int)tc_platform_default_backend());
    printf("stdout tty: %s\n", tc_stdout_is_tty() ? "yes" : "no");

    const tc_allocator* a = tc_allocator_default();
    void* p = a->alloc(a->ctx, 32, 8);
    if (!p) {
        printf("alloc failed: %s\n", tc_status_string(TC_ERR_NOMEM));
        return 1;
    }
    a->free(a->ctx, p, 32);

    printf("ok\n");
    return 0;
}
