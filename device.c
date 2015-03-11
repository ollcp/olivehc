/*
 * Devices and free blocks management.
 *
 * Author: Wu Bingzheng
 *
 */

#include "device.h"

/* init in device_conf_load() */
static ohc_ipbucket_t free_blocks;

static LIST_HEAD(devices);
static LIST_HEAD(deleted_devices);

static int device_badblock_percent;
static int device_check_270G;

/* this makes things complicated, but it's useful for saving
 * memory, in ohc_item_t and ohc_free_block_t. */
static idx_pointer_t device_indexs = IDX_POINTER_INIT();


static inline ohc_device_t *device_of_fblock(ohc_free_block_t *fblock)
{
	return idx_pointer_get(&device_indexs, fblock->device_index);
}

inline ohc_device_t *device_of_item(ohc_item_t *item)
{
	return idx_pointer_get(&device_indexs, item->device_index);
}


static inline void device_ipbucket_add(ohc_free_block_t *fblock)
{
	ipbucket_add(&free_blocks, &fblock->bucket_node, fblock->block_size);
}

static inline void device_ipbucket_update(ohc_free_block_t *fblock)
{
	ipbucket_update(&free_blocks, &fblock->bucket_node, fblock->block_size);
}

/* add a free block (with @offset and @size) into @device's order list,
 * before @base. */
static ohc_free_block_t *device_fblock_insert(ohc_device_t *device,
		struct list_head *base, off_t offset, size_t size)
{
	static ohc_slab_t free_block_slab = OHC_SLAB_INIT(ohc_free_block_t);

	ohc_free_block_t *fblock;

	fblock = slab_alloc(&free_block_slab);
	if(fblock == NULL) {
		return NULL;
	}
	fblock->fblock = 1;
	fblock->device_index = device->index;
	fblock->offset = offset;
	fblock->block_size = size;
	list_add_tail(&fblock->order_node, base);
	device_ipbucket_add(fblock);

	device->fblock_nr++;
	return fblock;
}

/* delete free block @fblock */
static void device_fblock_delete(ohc_free_block_t *fblock)
{
	ohc_device_t *device = device_of_fblock(fblock);
	device->fblock_nr--;
	ipbucket_del(&fblock->bucket_node);
	list_del(&fblock->order_node);
	slab_free(fblock);
}

/* remove the conf_device from conf_cycle.devices list,
 * and add it to the real devices list, so it becomes
 * the new device */
static void device_add(ohc_device_t *conf_device)
{
	ohc_device_t *d = conf_device;

	list_del(&d->dnode);
	list_add_tail(&d->dnode, &devices);

	INIT_LIST_HEAD(&d->order_head);
	conf_device->index = idx_pointer_add(&device_indexs, conf_device);
	if(d->capacity != 0) {
		if(device_fblock_insert(d, &d->order_head, 0, d->capacity) == NULL) {
			conf_device->kicked = 1;
			log_error_admin(0, "add device %s [NOMEM]", d->filename);
			return;
		}
	}

	/* other fields were set to zero, when malloc the conf_server */
}

static void device_update(ohc_device_t *d, ohc_device_t *conf_device)
{
	strcpy(d->filename, conf_device->filename);

	/* keep in order */
	list_del(&d->dnode);
	list_add_tail(&d->dnode, &devices);
}

static void device_delete(ohc_device_t *d)
{
	list_del(&d->dnode);

	if(d->kicked) {
		free(d);
	} else {
		d->deleted = 1;
		list_add(&d->dnode, &deleted_devices);
	}
}

static void device_destroy(ohc_device_t *d)
{
	struct list_head *p, *safe;
	ohc_free_block_t *fblock;
	ohc_item_t *item;
	int count = 0;

	list_for_each_safe(p, safe, &d->order_head) {
		fblock = list_entry(p, ohc_free_block_t, order_node);
		if(fblock->fblock) {
			device_fblock_delete(fblock);
		} else {
			item = list_entry(p, ohc_item_t, order_node);
			server_item_delete(item);
		}

		if(count++ >= LOOP_LIMIT) {
			break;
		}
	}

	/* we want to close the fd as soon as possible, while not
	 * need to wait for order_head empty. */
	if(d->used == 0 && d->fd != -1) {
		close(d->fd);
		d->fd = -1;
	}

	if(!list_empty(&d->order_head)) {
		return;
	}

	list_del(&d->dnode);
	idx_pointer_delete(&device_indexs, d->index);
	free(d);
}

/* Kick a device if it becomes bad.
 * But we still keep a 'kicked' device (.kicked=1),
 * for show status and re-load configure. */
static void device_kick(ohc_device_t *device)
{
	ohc_device_t *bad_dev = malloc(sizeof(ohc_device_t));
	if(bad_dev == NULL) {
		return;
	}

	*bad_dev = *device;

	bad_dev->kicked = 1;
	INIT_LIST_HEAD(&bad_dev->order_head);

	list_add(&bad_dev->dnode, &device->dnode);
	device_delete(device);
}

static ohc_device_t *device_search_inode(dev_t dev, ino_t inode,
		struct list_head *head, ohc_device_t *stop)
{
	struct list_head *p;
	ohc_device_t *d;

	list_for_each(p, head) {
		d = list_entry(p, ohc_device_t, dnode);
		if(d == stop) {
			return NULL;
		}
		if(d->inode == inode && d->dev == dev) {
			return d;
		}
	}
	return NULL;
}

static inline ohc_device_t *device_check_same(struct list_head *head, ohc_device_t *d)
{
	return device_search_inode(d->dev, d->inode, head, d);
}

int device_conf_check(ohc_conf_t *conf_cycle)
{
	struct list_head *p;
	ohc_device_t *d, *d2;
	const char *msg;
	struct stat filestat;
	int count = 0;

	if(list_empty(&conf_cycle->devices)) {
		log_error_admin(0, "you must set at least 1 device");
		return OHC_ERROR;
	}
	if(conf_cycle->device_badblock_percent >= 100) {
		log_error_admin(0, "device_badblock_percent must be less than 100");
		return OHC_ERROR;
	}

	list_for_each(p, &devices) {
		d = list_entry(p, ohc_device_t, dnode);
		d->conf = NULL;
	}

	/* deleted devices take slots in @device_indexs */
	list_for_each(p, &deleted_devices) {
		count++;
	}

	list_for_each(p, &conf_cycle->devices) {
		d = list_entry(p, ohc_device_t, dnode);

		if(++count >= DEVICES_LIMIT - 1) {
			msg = "too many devices";
			goto fail;
		}

		if(stat(d->filename, &filestat) < 0) {
			msg = "error in stat device";
			goto fail;
		}

		/* @st_dev and @st_ino work well for block-device too */
		d->dev = filestat.st_dev;
		d->inode = filestat.st_ino;

		d2 = device_check_same(&deleted_devices, d);
		if(d2 != NULL && d2->fd != -1) {
			msg = "in deleting, try later";
			goto fail;
		}

		/* check exist? */
		d2 = device_check_same(&devices, d);
		if(d2 != NULL) {
			if(d2->conf != NULL) {
				msg = "duplicate devices";
				goto fail;
			}

			d2->conf = d;
			d->conf = d2;
			continue;
		}

		/* new device, init it */

		if(device_check_same(&conf_cycle->devices, d)) {
			msg = "duplicate devices";
			goto fail;
		}

		d->fd = open(d->filename, O_RDWR);
		if(d->fd < 0) {
			msg = "error in open device";
			goto fail;
		}

		if(S_ISREG(filestat.st_mode)) {
			d->capacity = filestat.st_size & ~0x1FFL;

		} else if(S_ISBLK(filestat.st_mode)) {
			d->capacity = 0;
			if(ioctl(d->fd, BLKGETSIZE, &d->capacity) < 0) {
				msg = "error in ioctl device";
				goto fail;
			}
			d->capacity = d->capacity << 9; /* sector size is 512 */

		} else {
			msg = "error file type(only allow REGULAR and BLOCK)";
			goto fail;
		}

		if(d->capacity > 0x4020010000) {
			/* sendfile supports only 0x4020010000 */
			if(conf_cycle->device_check_270G) {
				msg = "file is too big(at most 0x4020010000)";
				goto fail;
			}
			d->capacity = 0x4020010000;
		}
	}
	return OHC_OK;

fail:
	/* the open FDs will be closed in device_conf_rollback() later */
	log_error_admin(errno, "%s of device '%s'", msg, d->filename);
	return OHC_ERROR;
}

void device_conf_load(ohc_conf_t *conf_cycle)
{
	struct list_head *p, *safe;
	ohc_device_t *d;

	/* init free_blocks. executes only once */
	static int first = 1;
	if(first) {
		first = 0;
		ipbucket_init(&free_blocks);
	}

	device_badblock_percent = conf_cycle->device_badblock_percent;
	device_check_270G = conf_cycle->device_check_270G;

	list_for_each_safe(p, safe, &devices) {
		d = list_entry(p, ohc_device_t, dnode);
		if(d->conf == NULL) {
			device_delete(d);
		}
	}

	list_for_each_safe(p, safe, &conf_cycle->devices) {
		d = list_entry(p, ohc_device_t, dnode);
		if(d->conf) {
			device_update(d->conf, d);
		} else {
			device_add(d);
		}
	}
}

void device_conf_rollback(ohc_conf_t *conf_cycle)
{
	struct list_head *p;
	ohc_device_t *d;

	list_for_each(p, &conf_cycle->devices) {
		d = list_entry(p, ohc_device_t, dnode);
		if(d->fd >= 0) {
			close(d->fd);
		}
	}
}

static void device_delete_item(ohc_device_t *d, struct list_head *p)
{
	if(p == &d->order_head) {
		return;
	}

	ohc_free_block_t *fblock = list_entry(p, ohc_free_block_t, order_node);
	if(fblock->fblock) {
		return;
	}

	ohc_item_t *item = list_entry(p, ohc_item_t, order_node);
	if(item->putting || item->used) { /* do not delete hot item */
		return;
	}
	server_item_delete(item);
}

/* delete items to extend free block, to make a big one */
int device_free_block_extend(size_t target)
{
	ohc_free_block_t *fblock;
	ohc_device_t *d;
	struct list_head *p, *next;
	size_t last = 0;
	int i;

	target = ipbucket_block_size(target);

	for(i = 0; i < LOOP_LIMIT; i++) {
		p = ipbucket_biggest(&free_blocks);
		if(p == NULL) {
			return OHC_ERROR;
		}

		fblock = list_entry(p, ohc_free_block_t, bucket_node);
		if(fblock->block_size >= target) {
			return OHC_OK;
		}

		if(fblock->block_size == last) {
			return OHC_ERROR;
		}
		last = fblock->block_size;

		d = device_of_fblock(fblock);
		if(d->deleted) {
			device_fblock_delete(fblock);
			continue;
		}

		/* device_delete_item() makes @fblock invalid, so
		 * we have to remember @next before call it. */
		next = fblock->order_node.next;
		device_delete_item(d, fblock->order_node.prev);
		device_delete_item(d, next);
	}
	return OHC_ERROR;
}

/* @server module call this to allocate a free block for a new item.
 * Set @item's @device and @offset member, and return free-block's
 * size, if alloc successfully.
 * Return 0 if fail. */
size_t device_get_free_block(ohc_item_t *item)
{
	ohc_free_block_t *fblock;
	ohc_device_t *device;
	struct list_head *p;
	size_t bsize;
	int try = 0;

try_again:
	p = ipbucket_get(&free_blocks, item->length);
	if(p == NULL) {
		return 0;
	}
	fblock = list_entry(p, ohc_free_block_t, bucket_node);
	device = device_of_fblock(fblock);

	if(device->deleted) {
		device_fblock_delete(fblock);
		if(try++ < LOOP_LIMIT) {
			goto try_again;
		}
		return 0;
	}

	bsize = ipbucket_block_size(item->length);
	if(fblock->block_size > bsize) {
		/* fblock is bigger than needed, so cut bsize from rear */

		item->offset = fblock->offset + fblock->block_size - bsize;
		list_add(&item->order_node, &fblock->order_node);

		fblock->block_size -= bsize;
		device_ipbucket_add(fblock);

	} else if(fblock->block_size == bsize) {
		/* fit exactly */
		item->offset = fblock->offset;

		list_add(&item->order_node, &fblock->order_node);
		device_fblock_delete(fblock);
	} else {
		/* should not be here */
		log_error_run(0, "oops!");
		exit(1);
	}

	device->item_nr++;
	device->consumed += bsize;
	item->device_index = device->index;
	return bsize;
}

/* @server module call this to free a free block when delete a item */
size_t device_return_free_block(ohc_item_t *item)
{
	ohc_free_block_t *prev = NULL, *next = NULL;
	ohc_device_t *device = device_of_item(item);
	off_t bsize;
	int badp;
	struct list_head *order = &item->order_node;
	int forward = 0, backward = 0;

	bsize = ipbucket_block_size(item->length);

	/* just delete item from order-list, if the device is deleted or bad. */
	if(device->deleted) {
		goto done;
	}
	if(item->badblock) {
		device->badblock += bsize;
		badp = device->badblock * 100 / device->capacity;
		if(badp > device_badblock_percent) {
			log_error_run(0, "kick device '%s', badblock:%ld(%d%%)",
					device->filename, device->badblock, badp);
			device_kick(device);
		}
		goto done;
	}

	/* ok, now recycle the item's block */

	if(order->prev != &device->order_head) {
		prev = list_entry(order->prev, ohc_free_block_t, order_node);
		forward = prev->fblock && (prev->offset + prev->block_size == item->offset);
	}
	if(order->next != &device->order_head) {
		next = list_entry(order->next, ohc_free_block_t, order_node);
		backward = next->fblock && (next->offset == item->offset + bsize);
	}

	if(forward && backward) {
		prev->block_size += bsize + next->block_size;
		device_ipbucket_update(prev);

		device_fblock_delete(next);

	} else if(forward) {
		prev->block_size += bsize;
		device_ipbucket_update(prev);

	} else if(backward) {
		next->offset -= bsize;
		next->block_size += bsize;
		device_ipbucket_update(next);

	} else {
		/* we don't care the return value here */
		device_fblock_insert(device, order, item->offset, bsize);
	}

	device->item_nr--;
	device->consumed -= bsize;

done:
	list_del(order);
	return bsize;
}

/* @format module call this to cut a free-block from the beginning
 * of the remaining space */
size_t device_cut_free_block(ohc_item_t *item)
{
	ohc_free_block_t *current;
	size_t bsize, step, gap;
	ohc_device_t *device = device_of_item(item);

	current = list_entry(device->order_head.prev, ohc_free_block_t, order_node);
	bsize = ipbucket_block_size(item->length);
	gap = item->offset - current->offset;
	step = bsize + gap;

	if(item->offset < current->offset || current->block_size < step) {
		log_error_run(0, "wrong olivehc dump device %s", device->filename);
		return 0;
	}

	if(gap > 0) {
		/* we don't care the return value here */
		device_fblock_insert(device, &current->order_node,
				current->offset, gap);
	}

	list_add_tail(&item->order_node, &current->order_node);

	/* we don't call ipbucket_update(current) here, while call it
	 * in device_load_post() later. */
	current->offset += step;
	current->block_size -= step;

	device->item_nr++;
	device->consumed += bsize;
	return bsize;
}

/* @format module call this, after finish loading items of a device,
 * to update the remaining space */
void device_load_post(ohc_device_t *device)
{
	ohc_free_block_t *current;
	current = list_entry(device->order_head.prev, ohc_free_block_t, order_node);

	if(current->block_size == 0) {
		device_fblock_delete(current);
	} else {
		device_ipbucket_update(current);
	}
}

void device_format_load(void)
{
	struct list_head *p;
	ohc_device_t *d;

	list_for_each(p, &devices) {
		d = list_entry(p, ohc_device_t, dnode);
		format_load_device(d);
	}
}

void device_format_store(void)
{
	struct list_head *p;
	ohc_device_t *d;
	unsigned short server_ports[SERVERS_LIMIT];

	server_dump_ports(server_ports);

	list_for_each(p, &devices) {
		d = list_entry(p, ohc_device_t, dnode);
		format_store_device(server_ports, d);
	}
}

/* regular routine, called by master thread */
void device_routine(void)
{
	struct list_head *p, *safep;
	ohc_device_t *d;

	list_for_each_safe(p, safep, &deleted_devices) {
		d = list_entry(p, ohc_device_t, dnode);
		device_destroy(d);
	}
}

void device_status(FILE *filp)
{
	struct list_head *p;
	ohc_device_t *d;

	fputs("\n+ device capacity consumed badblock status\n", filp);
	list_for_each(p, &devices) {
		d = list_entry(p, ohc_device_t, dnode);
		fprintf(filp, "++ %s %ld %ld %ld %s\n",
				d->filename, d->capacity, d->consumed,
				d->badblock, d->kicked ? "kicked" : "ok");
	}
}
