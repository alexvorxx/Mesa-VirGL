/*
 * Copyright 2013 Christoph Bumiller
 * SPDX-License-Identifier: MIT
 */

#include "nine_helpers.h"

static struct nine_range *
nine_range_pool_more(struct nine_range_pool *pool)
{
    struct nine_range *r = MALLOC(64 * sizeof(struct nine_range));
    int i;
    assert(!pool->free);

    if (pool->num_slabs == pool->num_slabs_max) {
        unsigned p = pool->num_slabs_max;
        unsigned n = pool->num_slabs_max * 2;
        if (!n)
            n = 4;
        pool->slabs = REALLOC(pool->slabs,
                              p * sizeof(struct nine_range *),
                              n * sizeof(struct nine_range *));
        pool->num_slabs_max = n;
    }
    pool->free = pool->slabs[pool->num_slabs++] = r;

    for (i = 0; i < 63; ++i, r = r->next)
        r->next = (struct nine_range *)
            ((uint8_t *)r + sizeof(struct nine_range));
    r->next = NULL;

    return pool->free;
}

static inline struct nine_range *
nine_range_pool_get(struct nine_range_pool *pool, int16_t bgn, int16_t end)
{
    struct nine_range *r = pool->free;
    if (!r)
        r = nine_range_pool_more(pool);
    assert(r);
    pool->free = r->next;
    r->bgn = bgn;
    r->end = end;
    return r;
}

static inline void
nine_ranges_coalesce(struct nine_range *r, struct nine_range_pool *pool)
{
    struct nine_range *n;

    while (r->next && r->end >= r->next->bgn) {
        n = r->next->next;
        r->end = (r->end >= r->next->end) ? r->end : r->next->end;
        nine_range_pool_put(pool, r->next);
        r->next = n;
    }
}

void
nine_ranges_insert(struct nine_range **head, int16_t bgn, int16_t end,
                   struct nine_range_pool *pool)
{
    struct nine_range *r, **pn = head;

    for (r = *head; r && bgn > r->end; pn = &r->next, r = r->next);

    if (!r || end < r->bgn) {
        *pn = nine_range_pool_get(pool, bgn, end);
        (*pn)->next = r;
    } else
    if (bgn < r->bgn) {
        r->bgn = bgn;
        if (end > r->end)
            r->end = end;
        nine_ranges_coalesce(r, pool);
    } else
    if (end > r->end) {
        r->end = end;
        nine_ranges_coalesce(r, pool);
    }
}
