/**
 *
 * A simple slab.
 *
 * Auther: Wu Bingzheng
 *
 **/

#ifndef _SLAB_H_
#define _SLAB_H_

#include "list.h"

typedef struct ohc_slab_s {
	struct hlist_head	block_head;
	unsigned		item_size;
} ohc_slab_t;

/* make sure: sizeof(type) >= sizeof(struct hlist_node) */
#define OHC_SLAB_INIT(type) \
	{HLIST_HEAD_INIT, sizeof(type)+sizeof(ohc_slab_t *)}

void *slab_alloc(ohc_slab_t *slab);
void slab_free(void *p);

#endif
