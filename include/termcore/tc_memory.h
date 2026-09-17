#ifndef TERMCORE_TC_MEMORY_H
#define TERMCORE_TC_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termcore/tc_export.h>

/* See docs/07-memory-and-errors.md §1.
 *
 * Every allocation goes through an allocator: the hot paths (present, event
 * polling) must not allocate, the cold paths (create, inject, probe) may.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct tc_allocator {
    void* (*alloc)(void* ctx, size_t size, size_t align);
    void  (*free)(void* ctx, void* ptr, size_t size);
    void* (*realloc)(void* ctx, void* ptr, size_t old_size, size_t new_size, size_t align);
    void* ctx;
} tc_allocator;

/* Process heap backed allocator. Never NULL. */
TC_API const tc_allocator* tc_allocator_default(void);

/* Test helper: wraps another allocator (NULL = default) and counts traffic so
 * suites can assert "no leak at the end of the case". */
typedef struct tc_alloc_counter {
    tc_allocator        allocator;   /* base: usable as tc_allocator* */
    const tc_allocator* backing;
    size_t              alloc_count;
    size_t              free_count;
    size_t              bytes_live;
    size_t              bytes_peak;
    uint32_t            reserved[2];
} tc_alloc_counter;

TC_API void   tc_alloc_counter_init(tc_alloc_counter* c, const tc_allocator* backing);
TC_API size_t tc_alloc_counter_live_bytes(const tc_alloc_counter* c);
TC_API bool   tc_alloc_counter_balanced(const tc_alloc_counter* c);

#ifdef __cplusplus
}
#endif

#endif /* TERMCORE_TC_MEMORY_H */
