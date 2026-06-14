/* rsharp/runtime/memory/arena.c */
#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#define DEFAULT_SLAB_SIZE (1 << 20)  /* 1 MiB */

static ArenaSlab *slab_create(size_t cap) {
    ArenaSlab *s = malloc(sizeof(ArenaSlab));
    assert(s && "arena: slab allocation failed (OOM)");
    s->base = malloc(cap);
    assert(s->base && "arena: slab buffer allocation failed (OOM)");
    s->cap  = cap;
    s->used = 0;
    s->next = NULL;
    return s;
}

Arena arena_create(size_t initial_capacity) {
    if (initial_capacity == 0) initial_capacity = DEFAULT_SLAB_SIZE;
    ArenaSlab *slab = slab_create(initial_capacity);
    return (Arena){
        .head        = slab,
        .first       = slab,
        .slab_size   = initial_capacity,
        .total_alloc = 0,
    };
}

/* Align up to a power-of-two boundary */
static inline size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

void *arena_alloc(Arena *a, size_t size, size_t align) {
    if (size == 0) return NULL;
    if (align == 0) align = 8; /* default to 8-byte alignment */

    ArenaSlab *s = a->head;
    size_t     aligned_used = align_up(s->used, align);

    /* Try current slab */
    if (size <= s->cap && aligned_used <= s->cap - size) {
        void *ptr = s->base + aligned_used;
        s->used   = aligned_used + size;
        a->total_alloc += size;
        return ptr;
    }

    /* Grow: add a new slab (at least as large as requested) */
    size_t new_cap = a->slab_size;
    if (size > new_cap) new_cap = align_up(size, 4096);
    ArenaSlab *ns = slab_create(new_cap);
    s->next = ns;
    a->head = ns;

    void *ptr = ns->base;
    ns->used  = size;
    a->total_alloc += size;
    return ptr;
}

void *arena_calloc(Arena *a, size_t count, size_t elem_size) {
    if (count > 0 && elem_size > SIZE_MAX / count) return NULL;
    size_t total = count * elem_size;
    void *ptr = arena_alloc(a, total, elem_size > 8 ? 8 : elem_size);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

char *arena_strdup(Arena *a, const char *s, size_t len) {
    char *buf = arena_alloc(a, len + 1, 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    return buf;
}

ArenaCheckpoint arena_checkpoint(const Arena *a) {
    return (ArenaCheckpoint){ .slab = a->head, .used = a->head->used };
}

void arena_rewind(Arena *a, ArenaCheckpoint cp) {
    /* Free slabs allocated after checkpoint */
    ArenaSlab *s = cp.slab->next;
    while (s) {
        ArenaSlab *next = s->next;
        free(s->base);
        free(s);
        s = next;
    }
    cp.slab->next = NULL;
    cp.slab->used = cp.used;
    a->head       = cp.slab;
}

void arena_reset(Arena *a) {
    /* Free all slabs except the first, reset used counters */
    ArenaSlab *s = a->first->next;
    while (s) {
        ArenaSlab *next = s->next;
        free(s->base);
        free(s);
        s = next;
    }
    a->first->next = NULL;
    a->first->used = 0;
    a->head        = a->first;
    a->total_alloc = 0;
}

void arena_destroy(Arena *a) {
    ArenaSlab *s = a->first;
    while (s) {
        ArenaSlab *next = s->next;
        free(s->base);
        free(s);
        s = next;
    }
    a->head = a->first = NULL;
}
