/*
 * Load items from device when starts, and store items to device when quits.
 *
 * Author: Wu Bingzheng
 *
 */

#include "format.h"

/*
 * Format of device file:
 *
 *     ohc_superblock_t
 *     server-listen-port[SERVERS_LIMIT]
 *     ohc_format_item_t[]
 *     items-body
 *     ...
 */


#define OHC_FM_MAGIC		0x2143484556494c4fL /* OLIVEHC! */
#define OHC_FM_VERSION		1

typedef struct {
	uint64_t	magic;
	int		version;
	uint64_t	checksum;
	long		item_nr;
} ohc_superblock_t;

#define SERVER_PORTS_SIZE (sizeof(unsigned short) * SERVERS_LIMIT)
#define OHC_FM_INFO_SIZE (sizeof(ohc_superblock_t) + SERVER_PORTS_SIZE)
#define OHC_FM_CHS_FEED 0x57eb0b4eecfeb465L

static uint64_t format_checksum(void *buf, size_t len)
{
	uint64_t *p = buf;
	uint64_t checksum = 0;
	int i;

	for(i = 0; i < len / sizeof(uint64_t); i++) {
		checksum ^= p[i];
	}
	return checksum;
}

int format_store_device(unsigned short *server_ports, ohc_device_t *device)
{
	struct list_head *p;
	ohc_superblock_t superb;
	ohc_item_t *item;
	ohc_free_block_t *fblock;
	ohc_format_item_t fm_item;
	ohc_server_t *server;

	FILE *filp = fdopen(device->fd, "r+");
	if(filp == NULL) {
		return OHC_ERROR;
	}

	/* init superblock */
	superb.magic = OHC_FM_MAGIC;
	superb.version = OHC_FM_VERSION;
	superb.checksum = 0;
	superb.item_nr = 0;

	/* servers */
	if(fseek(filp, sizeof(superb), SEEK_SET) < 0) {
		return OHC_ERROR;
	}
	if(fwrite(server_ports, SERVER_PORTS_SIZE, 1, filp) < 1) {
		return OHC_ERROR;
	}

	/* items */
	list_for_each(p, &device->order_head) {
		fblock = list_entry(p, ohc_free_block_t, order_node);
		if(fblock->fblock) {
			continue;
		}

		item = list_entry(p, ohc_item_t, order_node);
		if(!server_item_valid(item)) {
			continue;
		}

		server = server_of_item(item);
		if(!server->server_dump) {
			continue;
		}

		memcpy(fm_item.hash_id, item->hnode.id, 16);
		fm_item.expire = item->expire;
		fm_item.length = item->length;
		fm_item.headers_len = item->headers_len;
		fm_item.server_index = server->index;
		fm_item.offset = item->offset;
		if(fwrite(&fm_item, sizeof(ohc_format_item_t), 1, filp) < 1) {
			return OHC_ERROR;
		}

		superb.item_nr++;
	}

	if(superb.item_nr == 0) {
		return OHC_ERROR;
	}

	/* superblock */
	superb.checksum ^= format_checksum(&superb, sizeof(superb));
	superb.checksum ^= format_checksum(server_ports, SERVER_PORTS_SIZE);
	superb.checksum ^= OHC_FM_CHS_FEED;

	if(fseek(filp, 0, SEEK_SET) < 0) {
		return OHC_ERROR;
	}
	if(fwrite(&superb, sizeof(ohc_superblock_t), 1, filp) < 1) {
		return OHC_ERROR;
	}

	fclose(filp);
	return OHC_OK;
}

int format_load_device(ohc_device_t *device)
{
	unsigned char buffer[OHC_FM_INFO_SIZE];
	unsigned short *server_ports;
	ohc_superblock_t *superb;
	ohc_server_t *server;
	ohc_server_t *disk_servers[SERVERS_LIMIT];
	ohc_format_item_t fm_item;
	FILE *filp;
	long i;
	off_t override;
	int rc = OHC_ERROR;

	time_t now = timer_now(&master_timer);

	filp = fopen(device->filename, "r+");
	if(filp == NULL) {
		return OHC_ERROR;
	}
	if(fread(buffer, OHC_FM_INFO_SIZE, 1, filp) < 1) {
		goto out;
	}
	superb = (ohc_superblock_t *)&buffer[0];
	server_ports = (unsigned short *)(superb + 1);

	/* check */
	if(superb->magic != OHC_FM_MAGIC || superb->version != OHC_FM_VERSION) {
		goto out;
	}

	if(format_checksum(buffer, OHC_FM_INFO_SIZE) != OHC_FM_CHS_FEED) {
		goto out;
	}

	/* build @disk_servers */
	for(i = 0; i < SERVERS_LIMIT; i++) {
		if(server_ports[i] != 0) {
			disk_servers[i] = server_by_port(server_ports[i]);
		}
	}

	/* load items! */
	override = OHC_FM_INFO_SIZE + superb->item_nr * sizeof(ohc_format_item_t);
	if(fseek(filp, OHC_FM_INFO_SIZE, SEEK_SET) < 0) {
		goto out;
	}
	for(i = 0; i < superb->item_nr; i++) {
		if(fread(&fm_item, sizeof(ohc_format_item_t), 1, filp) < 1) {
			goto out;
		}

		if(fm_item.offset < override || fm_item.expire <= now) {
			continue;
		}

		server = disk_servers[fm_item.server_index];
		if(server == NULL) {
			continue;
		}

		server_load_fm_item(server, device, &fm_item);
	}
	device_load_post(device);

	/* clear the magic */
	if(fseek(filp, 0, SEEK_SET) < 0) {
		goto out;
	}
	if(fwrite("FeiLiWuShi", 10, 1, filp) < 1) {
		goto out;
	}
	rc = OHC_OK;

out:
	fclose(filp);
	return rc;
}
