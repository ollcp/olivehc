/**
 *
 * A simple slab.
 *
 * Auther: Wu Bingzheng
 *
 **/

/*
 * ohc_slab_t    slab_block_t   slab_block_t
 *  +----+    +------+<--\   +------+<--\
 *  |head|--->|      |---+-->|      |   |
 *  |    |    |      |   |   |      |   |
 *  |    |    |      |   |   |      |   |
 *  +----+    +======+   |   +======+   |  
 *            |leader|--/    |leader|--/  
 *    return->+ - - -|       + - - -|
 *            |      |       |      |    
 *            |      |       |      |   
 *            +------+       +------+  
 *            |leader|       |leader| 
 *            + - - -|       + - - -|
 *            |      |       |      |
 *            |      |       |      |
 *            +------+       +------+ 
 *            |      |       |      |
 *              ...            ...
 *            |      |       |      |
 *            +------+       +------+
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "slab.h"

typedef struct {
	struct hlist_node	block_node;
	struct hlist_head	item_head;
	ohc_slab_t		*slab;
	int			frees;
} slab_block_t;

static inline int slab_buckets(ohc_slab_t *slab)
{
	/* malloc use @brk if size<128K */
	return (128*1024 - sizeof(slab_block_t) - 100) / slab->item_size;
}

void *slab_alloc(ohc_slab_t *slab)
{
	slab_block_t *sblock;
	uintptr_t leader;
	struct hlist_node *p;
	int buckets;
	int i;

	if(hlist_empty(&slab->block_head)) {
		buckets = slab_buckets(slab);
		sblock = malloc(sizeof(slab_block_t) + slab->item_size * buckets);
		if(sblock == NULL) {
			return NULL;
		}

		sblock->slab = slab;
		sblock->frees = buckets;
		hlist_add_head(&sblock->block_node, &slab->block_head);
		INIT_HLIST_HEAD(&sblock->item_head);

		leader = (uintptr_t)sblock + sizeof(slab_block_t);
		for(i = 0; i < buckets; i++) {
			*((slab_block_t **)leader) = sblock;
			p = (struct hlist_node *)(leader + sizeof(slab_block_t *));
			hlist_add_head(p, &sblock->item_head);
			leader += slab->item_size;
		}

	} else {
		sblock = list_entry(slab->block_head.first, slab_block_t, block_node);
	}

	p = sblock->item_head.first;
	hlist_del(p);

	sblock->frees--;
	if(sblock->frees == 0) {
		/* if no free items, we throw the block away */
		hlist_del(&sblock->block_node);
	}

	return p;
}

void slab_free(void *p)
{
	uintptr_t leader = (uintptr_t)p - sizeof(slab_block_t **);
	slab_block_t *sblock = *((slab_block_t **)leader);
	ohc_slab_t *slab = sblock->slab;

	if(sblock->frees == 0) {
		/* if there WAS no free item in this block, we catch it again */
		hlist_add_head(&sblock->block_node, &slab->block_head);
	}

	hlist_add_head((struct hlist_node *)p, &sblock->item_head);

	sblock->frees++;
	if(sblock->frees == slab_buckets(slab) && sblock->block_node.next) {
		/* never free the first slab-block */
		hlist_del(&sblock->block_node);
		free(sblock);
	}
}
