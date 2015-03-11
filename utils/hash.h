/*
 * Linear dynamic hashing
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _HASH_H_
#define _HASH_H_

#include "list.h"

typedef struct ohc_hash_s ohc_hash_t;

typedef struct {
	unsigned char		id[16];
	struct hlist_node	node;
} ohc_hash_node_t;

ohc_hash_t *hash_init();
void hash_destroy(ohc_hash_t *hash);

void hash_add(ohc_hash_t *hash, ohc_hash_node_t *hnode, unsigned char *str, int len);
ohc_hash_node_t *hash_get(ohc_hash_t *hash, unsigned char *str, int len, unsigned char *hash_id);
void hash_del(ohc_hash_t *hash, ohc_hash_node_t *hnode);

#endif
