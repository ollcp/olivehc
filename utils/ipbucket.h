/*
 * Space management. Cut device into blocks in sizes
 * as (double and interpolation):
 *
 *   [1] [2] 3 [4] 5 6 7 [8] 10 12 14 [16] 20 24 28 [32] 40 48 56 [64] ...
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OHC_IPBUCKET_H_
#define _OHC_IPBUCKET_H_

#include "list.h"
#include "string.h"

/* we define ohc_item_t.length as uint32_t for saving memory,
 * so IPB_END must not be larger than 32. */

#define IPB_BEGIN	9
#define IPB_END		32
#define IPB_BUCKETS	((IPB_END - IPB_BEGIN) * 4 - 4 + 1)

#define IPB_1ST	(1L << IPB_BEGIN)
#define IPB_2ND	(IPB_1ST * 2)
#define IPB_3RD	(IPB_1ST * 3)
#define IPB_4TH	(IPB_1ST * 4)

typedef struct {
	struct list_head	queue[IPB_BUCKETS];
} ohc_ipbucket_t;


static inline void ipbucket_init(ohc_ipbucket_t *ipb)
{
	int i;
	for(i = 0; i < IPB_BUCKETS; i++) {
		INIT_LIST_HEAD(&ipb->queue[i]);
	}
}

static inline void ipbucket_destory(ohc_ipbucket_t *ipb)
{
	int i;
	/* only delete the list-heads from list, and
	 * the caller should handle the list-nodes. */
	for(i = 0; i < IPB_BUCKETS; i++) {
		list_del(&ipb->queue[i]);
	}
}

static inline size_t ipbucket_block_size(size_t size)
{
	size_t mask;

	if(size <= IPB_4TH) {
		mask = bit_mask(IPB_BEGIN);
	} else {
		mask = bit_mask(bit_highest(size) - 2);
	}
	return (size + mask) & ~mask;
}

static inline int ipbucket_index(size_t size, int up)
{
	int first, index, subidx, tail;

	if(size > (1L << IPB_END)) {
		return up ? -1 : IPB_BUCKETS - 1;
	}
	if(size <= IPB_4TH) {
		if(up) {
			if(size <= IPB_1ST) {
				return 0;
			} else if(size <= IPB_2ND) {
				return 1;
			} else if(size <= IPB_3RD) {
				return 2;
			} else {
				return 3;
			}
		} else {
			if(size < IPB_2ND) {
				return 0;
			} else if(size < IPB_3RD) {
				return 1;
			} else if(size < IPB_4TH) {
				return 2;
			} else {
				return 3;
			}
		}
	}

	first = bit_highest(size);
	index = (first - IPB_BEGIN) * 4 - 5;
	subidx = (size >> (first-2)) & 0x3;
	if(up) {
		tail = size & bit_mask(first - 2) ? 1 : 0;
	} else {
		tail = 0;
	}

	return index + subidx + tail;
}

static inline void ipbucket_add(ohc_ipbucket_t *ipb, struct list_head *node, size_t size)
{
	struct list_head *p = &ipb->queue[ipbucket_index(size, 0)];

	if(size == ipbucket_block_size(size)) {
		list_add(node, p);
	} else {
		list_add_tail(node, p);
	}
}

static inline void ipbucket_del(struct list_head *node)
{
	if(node->prev) {
		list_del(node);
	}
}

static inline void ipbucket_update(ohc_ipbucket_t *ipb, struct list_head *node, size_t size)
{
	list_del(node);
	ipbucket_add(ipb, node, size);
}

static inline struct list_head *ipbucket_get(ohc_ipbucket_t *ipb, size_t size)
{
	struct list_head *p;
	int index = ipbucket_index(size, 1);

	if(index == -1) {
		return NULL;
	}

	for(; index < IPB_BUCKETS; index++) {
		if(!list_empty(&ipb->queue[index])) {
			p = ipb->queue[index].next;
			list_del(p);
			return p;
		}
	}
	return NULL;
}

/* return an almost biggest block, but don't unlink it. */
static inline struct list_head *ipbucket_biggest(ohc_ipbucket_t *ipb)
{
	int index;
	for(index = IPB_BUCKETS - 1; index >= 0; index--) {
		if(!list_empty(&ipb->queue[index])) {
			return ipb->queue[index].prev;
		}
	}
	return NULL;
}

#endif
