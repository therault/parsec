/*
 * Copyright (c) 2009-2016 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */
#ifndef __USE_ARENA_H__
#define __USE_ARENA_H__

#include "parsec_config.h"
#include "parsec_internal.h"
#if defined(PARSEC_HAVE_STDDEF_H)
#include <stddef.h>
#endif  /* PARSEC_HAVE_STDDEF_H */
#include "parsec/debug.h"

#include <parsec/sys/atomic.h>
#include "parsec/class/lifo.h"

BEGIN_C_DECLS

/**
 * Maximum amount of memory each arena is allowed to manipulate.
 */
extern size_t parsec_arena_max_allocated_memory;

/**
 * Maximum amount of memory cached on each arena.
 */
extern size_t parsec_arena_max_cached_memory;

#define PARSEC_ALIGN(x,a,t) (((x)+((t)(a)-1)) & ~(((t)(a)-1)))
#define PARSEC_ALIGN_PTR(x,a,t) ((t)PARSEC_ALIGN((uintptr_t)x, a, uintptr_t))
#define PARSEC_ALIGN_PAD_AMOUNT(x,s) ((~((uintptr_t)(x))+1) & ((uintptr_t)(s)-1))

struct parsec_arena_s {
    parsec_lifo_t          area_lifo;
    size_t                alignment;     /* alignment to be respected, elem_size should be >> alignment, prefix size is
                                          the minimum alignment */
    size_t                elem_size;     /* size of one element (unpacked in memory, aka extent) */
    parsec_datatype_t      opaque_dtt;    /* the appropriate type for the network engine to send an element */
    volatile int32_t      used;           /* elements currently allocated from the arena */
    int32_t               max_used;       /* maximum size of the arena in elements */
    volatile int32_t      released;       /* elements currently released but still cached in the freelist */
    int32_t               max_released;   /* when more that max elements are released, they are really freed instead of joining the lifo */
    /* some host hardware requires special allocation functions (Cuda, pinning,
     * Open CL, ...). Defaults are to use C malloc/free */
    parsec_data_allocate_t data_malloc;
    parsec_data_free_t     data_free;
};

struct parsec_arena_chunk_s {
    parsec_list_item_t item;               /* chaining of this chunk when in an arena's free list.
                                           *   SINGLETON when ( (not in free list) and (in debug mode) ) */
    uint32_t          count;
    parsec_arena_t    *origin;
    void             *data;
};

/* for SSE, 16 is mandatory, most cache are 64 bit aligned */
#define PARSEC_ARENA_ALIGNMENT_64b 8
#define PARSEC_ARENA_ALIGNMENT_INT sizeof(int)
#define PARSEC_ARENA_ALIGNMENT_PTR sizeof(void*)
#define PARSEC_ARENA_ALIGNMENT_SSE 16
#define PARSEC_ARENA_ALIGNMENT_CL1 64

/**
 * Constructor for the arena class. By default this constructor
 * does not enable any caching, thus it behaves more like a
 * convenience wrapper around malloc/free than a freelist.
 */
int parsec_arena_construct(parsec_arena_t* arena,
                          size_t elem_size,
                          size_t alignment,
                          parsec_datatype_t opaque_dtt);
/**
 * Extended constructor for the arena class. It enabled the
 * caching support up to max_released number of elements,
 * and prevents the freelist to handle more than max_used
 * active elements at the same time.
 */
int parsec_arena_construct_ex(parsec_arena_t* arena,
                             size_t elem_size,
                             size_t alignment,
                             parsec_datatype_t opaque_dtt,
                             size_t max_used,
                             size_t max_released);
/**
 * Release the arena. All the memory allocated for the elements
 * by the arena is released, but not the parsec_data_copy_t and
 * parsec_data_t allocated to support the arena.
 */
void parsec_arena_destruct(parsec_arena_t* arena);

parsec_data_copy_t *parsec_arena_get_copy(parsec_arena_t *arena, size_t count, int device);
void parsec_arena_release(parsec_data_copy_t* ptr);

END_C_DECLS

#endif /* __USE_ARENA_H__ */

