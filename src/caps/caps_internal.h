#ifndef TERMCORE_CAPS_CAPS_INTERNAL_H
#define TERMCORE_CAPS_CAPS_INTERNAL_H

/* Internals shared by the capability layer sources. Never installed. */
#include <stdbool.h>
#include <stdint.h>
#include <termcore/tc_caps.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of defined capability bits (0 .. TC_CAP_BIT_COUNT-1). */
#define TC_CAP_BIT_COUNT 26

/* Mask over all defined bits. */
#define TC_CAPS_BIT_MASK ((1ull << TC_CAP_BIT_COUNT) - 1u)

/* Per-term capability state, owned by the control layer (docs/03 §2).
 *
 * `base` is the conclusion produced by the detection chain (environment
 * inference -> built-in TERM database -> conservative default; DA query and
 * terminfo land later). `caps` is what every layer actually reads: it equals
 * `base` until an override (API or profile) is applied, at which point the
 * override wins for the fields it touches (docs/03 §2, §9).
 *
 * `origin[]` / `confirmed_mask` / `decided_at_ms[]` track the per-bit
 * provenance that the profile snapshot reports (docs/03 §10.1); the `base_`
 * twins remember the detection-chain provenance so clearing an override can
 * restore it exactly.
 */
typedef struct tc_caps_state {
    tc_caps base;            /* detection-chain conclusion */
    tc_caps caps;            /* effective conclusion (override-aware) */
    bool    has_override;    /* a full override / app write is in force */
    bool    query_allowed;   /* DA query is permitted for this session */
    uint8_t origin[TC_CAP_BIT_COUNT];        /* tc_caps_origin, effective */
    uint8_t base_origin[TC_CAP_BIT_COUNT];   /* provenance of `base` */
    uint32_t confirmed_mask;                 /* user_confirmed per bit */
    uint32_t reserved2;
    int64_t  decided_at_ms[TC_CAP_BIT_COUNT];      /* effective */
    int64_t  base_decided_at_ms[TC_CAP_BIT_COUNT]; /* detection time */
} tc_caps_state;

/* Allocates and initialises the state: runs the detection chain minus DA /
 * terminfo (those arrive in later stages) and stores the result in *out.
 * Uses `alloc` for the struct; on failure returns TC_ERR_NOMEM and leaves
 * *out NULL. */
tc_status tc_caps_state_create(const tc_allocator* alloc, const tc_term_options* opt,
                               tc_caps_state** out);

/* Frees the state. NULL is a no-op. */
void tc_caps_state_destroy(const tc_allocator* alloc, tc_caps_state* s);

/* Detection chain steps, exposed for unit tests (docs/03 §2).
 *
 * tc_caps_from_env is public; the builtin TERM database and the conservative
 * default are internal. `term_name` is TERM (may be NULL/empty). */
tc_status tc_caps_from_builtin(const char* term_name, tc_caps* out);
void      tc_caps_conservative_default(tc_caps* out);

/* Control-layer gating (docs/03 §7, §9, §10.2): may the runtime feature run
 * under `caps`? RAW_MODE and CAPTURE_CTRL_C are never gated; `caps` NULL (a
 * term without caps state) never gates either. */
bool tc_caps_allows_feature(const tc_caps* caps, tc_feature f);

/* Mouse degradation (docs/03 §7): the highest mode `caps` supports, capped at
 * the application's request — MOTION -> DRAG -> CLICK -> OFF. */
tc_mouse_mode tc_caps_effective_mouse(const tc_caps* caps, tc_mouse_mode requested);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_CAPS_CAPS_INTERNAL_H */
