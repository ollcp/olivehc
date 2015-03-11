/*
 * Linear dynamic hashing
 *
 * Author: Wu Bingzheng
 *
 */

#include <openssl/md5.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "hash.h"
#include "list.h"

typedef int hindex_t;

/* hash bucket size range from 2^4 to 2^28*/
#define HASH_BUCKET_SIZE_BEGIN	(1<<4)
#define HASH_BUCKET_SIZE_MAX	(1<<28)

struct ohc_hash_s {
	struct hlist_head	*buckets;
	hindex_t		bucket_size;
	long			items;

	/* previous buckets, in split process */
	struct hlist_head	*prev_buckets;
	/* split pointer in linear hashing */
	hindex_t		split;
};

inline static int md5_equal(unsigned char *id1, unsigned char *id2)
{
	uint64_t *p = (uint64_t *)id1;
	uint64_t *q = (uint64_t *)id2;
	return (*p == *q) && (*(p+1) == *(q+1));
}

inline static hindex_t hash_index(ohc_hash_t *hash, unsigned char *id)
{
	uint64_t *p = (uint64_t *)id;
	return (*p ^ *(p+1)) & (hash->bucket_size - 1);
}

ohc_hash_t *hash_init(void)
{
	ohc_hash_t *hash;

	hash = malloc(sizeof(ohc_hash_t));
	if(hash == NULL) {
		return NULL;
	}

	hash->items = 0;
	hash->split = 0;
	hash->prev_buckets = NULL;
	hash->bucket_size = HASH_BUCKET_SIZE_BEGIN;

	/* @calloc do the same thing with INIT_HLIST_HEAD. */
	hash->buckets = calloc(hash->bucket_size, sizeof(struct hlist_head));
	if(hash->buckets == NULL) {
		free(hash);
		return NULL;
	}

	return hash;
}

void hash_destroy(ohc_hash_t *hash)
{
	free(hash->buckets);
	if(hash->prev_buckets) {
		free(hash->prev_buckets);
	}
	free(hash);
}

/* expansion: double the hash buckets */
static void hash_expansion(ohc_hash_t *hash)
{
	ohc_hash_node_t *hnode;
	struct hlist_node *p, *safe;
	void *newb;

#define HASH_COLLISIONS 10
	/* expansion */
	if(hash->items / hash->bucket_size >= HASH_COLLISIONS
			&& hash->prev_buckets == NULL
			&& hash->bucket_size < HASH_BUCKET_SIZE_MAX) {

		newb = calloc(hash->bucket_size * 2, sizeof(struct hlist_head));
		if(newb == NULL) {
			/* if calloc fails, do nothing */
			return;
		}
		hash->bucket_size *= 2;
		hash->prev_buckets = hash->buckets;
		hash->buckets = newb;
		hash->split = 0;
	}

	/* split a bucket */
	if(hash->prev_buckets != NULL) {

		for(p = hash->prev_buckets[hash->split].first; p; p = safe) {
			safe = p->next;
			hlist_del(p);

			hnode = list_entry(p, ohc_hash_node_t, node);
			hlist_add_head(p, &hash->buckets[hash_index(hash, hnode->id)]);
		}

		hash->split++;
		if(hash->split == hash->bucket_size / 2) {
			/* expansion finish */
			free(hash->prev_buckets);
			hash->prev_buckets = NULL;
		}
	}
}

void hash_add(ohc_hash_t *hash, ohc_hash_node_t *hnode, unsigned char *str, int len)
{
	if(str) {
		MD5(str, len, hnode->id);
	}

	hlist_add_head(&hnode->node, &hash->buckets[hash_index(hash, hnode->id)]);

	hash->items++;
	hash_expansion(hash);
}

static ohc_hash_node_t *hash_search(struct hlist_head *slot, unsigned char *id)
{
	struct hlist_node *p;
	ohc_hash_node_t *hnode;

	hlist_for_each(p, slot) {
		hnode = list_entry(p, ohc_hash_node_t, node);
		if(md5_equal(hnode->id, id)) {
			return hnode;
		}
	}

	return NULL;
}

ohc_hash_node_t *hash_get(ohc_hash_t *hash, unsigned char *str, int len, unsigned char *hash_id)
{
	unsigned char id_buf[16];
	unsigned char *id;
	hindex_t index;
	hindex_t pbsize;
	ohc_hash_node_t *ret;

	hash_expansion(hash);

	id = hash_id ? hash_id : id_buf;
	MD5(str, len, id);
	index = hash_index(hash, id);

	ret = hash_search(&hash->buckets[index], id);
	if(ret != NULL) {
		return ret;
	}

	if(hash->prev_buckets != NULL) {
		pbsize = hash->bucket_size / 2;
		if(index >= pbsize) {
			index -= pbsize;
		}
		return hash_search(&hash->prev_buckets[index], id);
	}

	return NULL;
}

void hash_del(ohc_hash_t *hash, ohc_hash_node_t *hnode)
{
	hlist_del(&hnode->node);
	hash->items--;
}
