/* rsharp/runtime/memory/arena.h + arena.c
 * Arena Allocator — zero-overhead bump allocator
 * Language: C11
 *
 * Usage:
 *   Arena a = arena_create(1 << 20);   // 1 MB
 *   char *buf = arena_alloc(&a, 256, 1);
 *   arena_reset(&a);                   // free all at once
 *   arena_destroy(&a);
 */

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Slab: one contiguous memory block */
typedef struct ArenaSlab {
    uint8_t           *base;
    size_t             cap;
    size_t             used;
    struct ArenaSlab  *next;
} ArenaSlab;

/* Arena: linked list of slabs */
typedef struct Arena {
    ArenaSlab *head;       /* current active slab     */
    ArenaSlab *first;      /* first slab (for reset)  */
    size_t     slab_size;  /* default slab capacity   */
    size_t     total_alloc;/* cumulative bytes served */
} Arena;

/* Checkpoint for partial rewind */
typedef struct ArenaCheckpoint {
    ArenaSlab *slab;
    size_t     used;
} ArenaCheckpoint;

Arena             arena_create(size_t initial_capacity);
void             *arena_alloc(Arena *a, size_t size, size_t align);
void             *arena_calloc(Arena *a, size_t count, size_t elem_size);
char             *arena_strdup(Arena *a, const char *s, size_t len);
ArenaCheckpoint   arena_checkpoint(const Arena *a);
void              arena_rewind(Arena *a, ArenaCheckpoint cp);
void              arena_reset(Arena *a);
void              arena_destroy(Arena *a);
