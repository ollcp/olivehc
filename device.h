/*
 * Devices and free blocks management.
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OHC_STORE_H_
#define _OHC_STORE_H_

#include "olivehc.h"

struct ohc_device_s {
	unsigned	deleted:1;
	unsigned	kicked:1;

	int		fd;
	int		index;
	int		used;
	char		filename[PATH_LENGTH];
	dev_t		dev;
	ino_t		inode;
	long		item_nr;
	long		fblock_nr;
	size_t		capacity;
	size_t		consumed;
	size_t		badblock;

	struct list_head	order_head;
	struct list_head	dnode;

	struct ohc_device_s	*conf;
};

typedef struct {
	/* @order_node and @fblock must be together, to
	 * distinguish ohc_item_t and ohc_free_block_t. */
	struct list_head	order_node;
	unsigned		fblock:1;

	short			device_index;

	/* since sendfile(2) supports only 0x4020010000, so 40bits is enough */
	unsigned long		offset:40;

	off_t			block_size;
	struct list_head	bucket_node;
} ohc_free_block_t;

#define DEVICES_LIMIT IPT_ARRAY_SIZE

ohc_device_t *device_of_item(ohc_item_t *item);

int device_conf_check(ohc_conf_t *conf_cycle);
void device_conf_load(ohc_conf_t *conf_cycle);
void device_conf_rollback(ohc_conf_t *conf_cycle);

int device_free_block_extend(size_t target);
size_t device_get_free_block(ohc_item_t *item);
size_t device_return_free_block(ohc_item_t *item);
size_t device_cut_free_block(ohc_item_t *item);
void device_load_post(ohc_device_t *device);

void device_format_load(void);
void device_format_store(void);
void device_routine(void);
void device_status(FILE *filp);

#endif
