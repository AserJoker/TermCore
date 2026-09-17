#include <termcore/tc_memory.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Default allocator: plain process heap.
 *
 * TODO(termcore): honour `align` for over-aligned requests
 * (aligned_alloc / _aligned_malloc). malloc already satisfies every
 * fundamental alignment, which is all the current layers ask for.
 * ------------------------------------------------------------------------ */
static void* sys_alloc(void* ctx, size_t size, size_t align) {
    (void)ctx;
    (void)align;
    return malloc(size);
}

static void sys_free(void* ctx, void* ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

static void* sys_realloc(void* ctx, void* ptr, size_t old_size, size_t new_size, size_t align) {
    (void)ctx;
    (void)old_size;
    (void)align;
    return realloc(ptr, new_size);
}

static const tc_allocator g_default_allocator = {
    sys_alloc,
    sys_free,
    sys_realloc,
    NULL
};

const tc_allocator* tc_allocator_default(void) {
    return &g_default_allocator;
}

/* --------------------------------------------------------------------------
 * Counting allocator (test helper)
 *
 * Layout of a counted block:
 *
 *   base                                  returned pointer
 *   |                                     |
 *   v                                     v
 *   [ size ][ ... padding ... ][ pad ][ user data ... ]
 *
 * `size` lives at base, `pad` lives immediately before the returned pointer at
 * a fixed offset, so free/realloc can recover both without extra bookkeeping.
 * ------------------------------------------------------------------------ */
#define TC_COUNTED_MIN_PAD (2u * sizeof(size_t))
#define TC_COUNTED_PAD(align) \
    (((TC_COUNTED_MIN_PAD + (align) - 1u) / (align)) * (align))

static void counted_write_header(unsigned char* base, size_t pad, size_t size) {
    memcpy(base, &size, sizeof(size));
    memcpy(base + pad - sizeof(size_t), &pad, sizeof(size_t));
}

static size_t counted_read_pad(void* p) {
    size_t pad = 0;
    memcpy(&pad, (unsigned char*)p - sizeof(size_t), sizeof(size_t));
    return pad;
}

static size_t counted_read_size(void* p, size_t pad) {
    size_t size = 0;
    memcpy(&size, (unsigned char*)p - pad, sizeof(size));
    return size;
}

static void* counter_alloc(void* ctx, size_t size, size_t align) {
    tc_alloc_counter* c = (tc_alloc_counter*)ctx;
    size_t a = align ? align : sizeof(void*);
    size_t pad = TC_COUNTED_PAD(a);
    unsigned char* base;

    base = (unsigned char*)c->backing->alloc(c->backing->ctx, pad + size, a);
    if (!base) return NULL;

    counted_write_header(base, pad, size);

    c->alloc_count++;
    c->bytes_live += size;
    if (c->bytes_live > c->bytes_peak) c->bytes_peak = c->bytes_live;

    return base + pad;
}

static void counter_free(void* ctx, void* ptr, size_t size) {
    tc_alloc_counter* c = (tc_alloc_counter*)ctx;
    size_t pad;
    size_t actual;

    (void)size;
    if (!ptr) return;

    pad = counted_read_pad(ptr);
    actual = counted_read_size(ptr, pad);

    c->free_count++;
    c->bytes_live -= (actual < c->bytes_live) ? actual : c->bytes_live;

    c->backing->free(c->backing->ctx, (unsigned char*)ptr - pad, pad + actual);
}

static void* counter_realloc(void* ctx, void* ptr, size_t old_size, size_t new_size, size_t align) {
    tc_alloc_counter* c = (tc_alloc_counter*)ctx;
    size_t a = align ? align : sizeof(void*);
    size_t pad;
    size_t actual;
    unsigned char* base;
    unsigned char* grown;

    (void)old_size;
    if (!ptr) return counter_alloc(ctx, new_size, align);

    pad = counted_read_pad(ptr);
    actual = counted_read_size(ptr, pad);
    base = (unsigned char*)ptr - pad;

    grown = (unsigned char*)c->backing->alloc(c->backing->ctx, pad + new_size, a);
    if (!grown) return NULL;

    memcpy(grown + pad, base + pad, actual < new_size ? actual : new_size);
    c->backing->free(c->backing->ctx, base, pad + actual);

    counted_write_header(grown, pad, new_size);

    c->bytes_live = (c->bytes_live - actual) + new_size;
    if (c->bytes_live > c->bytes_peak) c->bytes_peak = c->bytes_live;

    return grown + pad;
}

void tc_alloc_counter_init(tc_alloc_counter* c, const tc_allocator* backing) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->backing = backing ? backing : tc_allocator_default();
    c->allocator.alloc   = counter_alloc;
    c->allocator.free    = counter_free;
    c->allocator.realloc = counter_realloc;
    c->allocator.ctx     = c;
}

size_t tc_alloc_counter_live_bytes(const tc_alloc_counter* c) {
    return c ? c->bytes_live : 0;
}

bool tc_alloc_counter_balanced(const tc_alloc_counter* c) {
    return c && c->bytes_live == 0 && c->alloc_count == c->free_count;
}
