/*
 * Manage servers and items. Create/destroy/update servers,
 * get/put/delete items, etc.
 *
 * Author: Wu Bingzheng
 *
 */

#include "server.h"


static ohc_slab_t item_slab = OHC_SLAB_INIT(ohc_item_t);

static LIST_HEAD(servers);
static LIST_HEAD(deleted_servers);

static LIST_HEAD(shared_lru_head);

/* this makes things complicated, but it's useful for saving
 * memory, in ohc_item_t. */
static idx_pointer_t server_indexs = IDX_POINTER_INIT();

ohc_server_t *server_of_item(ohc_item_t *item)
{
	return idx_pointer_get(&server_indexs, item->server_index);
}

void server_dump_ports(unsigned short *ports)
{
	struct list_head *p;
	ohc_server_t *s;

	bzero(ports, sizeof(unsigned short) * SERVERS_LIMIT);
	list_for_each(p, &servers) {
		s = list_entry(p, ohc_server_t, snode);
		ports[s->index] = s->listen_port;
	}
}

static void server_listen_close(ohc_server_t *s)
{
	epoll_del(master_epoll_fd, s->listen_fd);
	close(s->listen_fd);
}

static int server_listen_start(ohc_server_t *s)
{
	tcp_listen(s->listen_fd);
	return epoll_add_read(master_epoll_fd, s->listen_fd,
			(void *)((uintptr_t)s | EVENT_TYPE_LISTEN));
}

/* we don't check their return values */
static void server_listen_set(ohc_server_t *s)
{
	if(s->sndbuf != 0) {
		set_sndbuf(s->listen_fd, s->sndbuf);
	}
	if(s->rcvbuf != 0) {
		set_rcvbuf(s->listen_fd, s->rcvbuf);
	}
	if(s->request_timeout != 60) {
		set_defer_accept(s->listen_fd, s->request_timeout);
	}
}

/* we don't check their return values */
static void server_listen_update(ohc_server_t *s, ohc_server_t *conf_server)
{
	if(conf_server->sndbuf != s->sndbuf) {
		set_sndbuf(s->listen_fd, conf_server->sndbuf);
		s->sndbuf = conf_server->sndbuf;
	}
	if(conf_server->rcvbuf != s->rcvbuf) {
		set_rcvbuf(s->listen_fd, conf_server->rcvbuf);
		s->rcvbuf = conf_server->rcvbuf;
	}
	if(conf_server->request_timeout != s->request_timeout) {
		set_defer_accept(s->listen_fd, conf_server->request_timeout);
		s->request_timeout = conf_server->request_timeout;
	}
}


/* remove the conf_server from conf_cycle.servers list,
 * and add it to the real servers list, so it becomes
 * the new server */
static void server_create(ohc_server_t *conf_server)
{
	list_del(&conf_server->snode);
	list_add_tail(&conf_server->snode, &servers);

	server_listen_start(conf_server);
	server_listen_set(conf_server);
	INIT_LIST_HEAD(&conf_server->lru_head);
	INIT_LIST_HEAD(&conf_server->passby_lru_head);
	conf_server->index = idx_pointer_add(&server_indexs, conf_server);

	/* other fields were set to zero, when malloc the conf_server */
}

/* move the server to deleted_servers list, in order
 * to free its items bit by bit. */
static void server_delete(ohc_server_t *s)
{
	s->deleted = 1;
	server_listen_close(s);
	list_del(&s->snode);
	list_add(&s->snode, &deleted_servers);
}

/* update a server by @conf_server */
static void server_update(ohc_server_t *s, ohc_server_t *conf_server)
{
	/* make in order */
	list_del(&s->snode);
	list_add_tail(&s->snode, &servers);

	if(conf_server->access_filp) {
		fclose(s->access_filp);
		s->access_filp = conf_server->access_filp;
		strcpy(s->access_log, conf_server->access_log);
	}

	s->capacity = conf_server->capacity;
	s->send_timeout = conf_server->send_timeout;
	s->recv_timeout = conf_server->recv_timeout;
	s->item_max_size = conf_server->item_max_size;
	s->passby_enable = conf_server->passby_enable;
	s->passby_begin_item_nr = conf_server->passby_begin_item_nr;
	s->passby_begin_consumed = conf_server->passby_begin_consumed;
	s->passby_limit_nr = conf_server->passby_limit_nr;
	s->passby_expire = conf_server->passby_expire;
	s->keepalive_timeout = conf_server->keepalive_timeout;
	s->server_dump = conf_server->server_dump;
	s->shutdown_if_not_store = conf_server->shutdown_if_not_store;
	s->key_include_query = conf_server->key_include_query;
	s->key_include_host = conf_server->key_include_host;
	s->key_include_ohc_key = conf_server->key_include_ohc_key;
	s->expire_default = conf_server->expire_default;
	s->expire_force = conf_server->expire_force;
	s->status_period = conf_server->status_period;
	server_listen_update(s, conf_server);
}

static ohc_server_t *server_search_port(unsigned short port,
		struct list_head *head, ohc_server_t *stop)
{
	struct list_head *p;
	ohc_server_t *s;

	list_for_each(p, head) {
		s = list_entry(p, ohc_server_t, snode);
		if(s == stop) {
			return NULL;
		}
		if(s->listen_port == port) {
			return s;
		}
	}
	return NULL;
}

ohc_server_t *server_by_port(unsigned short port)
{
	return server_search_port(port, &servers, NULL);
}

static ohc_server_t *server_check_same(struct list_head *head, ohc_server_t *s)
{
	return server_search_port(s->listen_port, head, s);
}

int server_conf_check(ohc_conf_t *conf_cycle)
{
	struct list_head *p;
	ohc_server_t *s, *s2;
	const char *msg;
	int count = 0;

	if(list_empty(&conf_cycle->servers)) {
		log_error_admin(0, "you must set at least 1 server");
		return OHC_ERROR;
	}

	list_for_each(p, &servers) {
		s = list_entry(p, ohc_server_t, snode);
		s->conf = NULL;
	}

	/* deleted servers take slots in @server_indexs */
	list_for_each(p, &deleted_servers) {
		count++;
	}

	list_for_each(p, &conf_cycle->servers) {
		s = list_entry(p, ohc_server_t, snode);

		if(++count >= SERVERS_LIMIT - 1) {
			msg = "too many servers";
			goto fail;
		}

		if(s->send_timeout <= 0) {
			msg = "send_timeout must be positive";
			goto fail;
		}
		if(s->recv_timeout <= 0) {
			msg = "recv_timeout must be positive";
			goto fail;
		}
		if(s->status_period == 0) {
			msg = "status_period must be positive";
			goto fail;
		}
		/* we don't check sndbuf and rcvbuf */

		s2 = server_check_same(&servers, s);
		if(s2 != NULL) {
			if(s2->conf != NULL) {
				msg = "duplicated listen port";
				goto fail;
			}
			s->conf = s2;
			s2->conf = s;
		} else {
			/* new server, so init it */

			if(server_check_same(&conf_cycle->servers, s)) {
				msg = "duplicated listen port";
				goto fail;
			}

			s->listen_fd = tcp_bind(s->listen_port);
			if(s->listen_fd < 0) {
				msg = "error in bind port";
				goto fail;
			}

			s->hash = hash_init();
			if(s->hash == NULL) {
				msg = "no mem when init hash";
				goto fail;
			}
		}

		if(s->conf == NULL || strcmp(s->access_log, s->conf->access_log)) {
			s->access_filp = fopen(s->access_log, "a");
			if(s->access_filp == NULL) {
				msg = "error in open log file";
				goto fail;
			}
		}
	}

	return OHC_OK;
fail:
	log_error_admin(errno, "%s in server %d", msg, s->listen_port);
	return OHC_ERROR;
}

void server_conf_load(ohc_conf_t *conf_cycle)
{
	struct list_head *p, *safe;
	ohc_server_t *s;

	list_for_each_safe(p, safe, &servers) {
		s = list_entry(p, ohc_server_t, snode);
		if(s->conf == NULL) {
			server_delete(s);
		}
	}

	list_for_each_safe(p, safe, &conf_cycle->servers) {
		s = list_entry(p, ohc_server_t, snode);
		if(s->conf) {
			server_update(s->conf, s);
		} else {
			server_create(s);
		}
	}
}

void server_conf_rollback(ohc_conf_t *conf_cycle)
{
	struct list_head *p;
	ohc_server_t *s;

	list_for_each(p, &conf_cycle->servers) {
		s = list_entry(p, ohc_server_t, snode);
		if(s->access_filp) {
			fclose(s->access_filp);
		}
		if(s->listen_fd >= 0) {
			close(s->listen_fd);
		}
		if(s->hash) {
			hash_destroy(s->hash);
		}
	}
}

void server_stop_service(void)
{
	struct list_head *p;
	ohc_server_t *s;

	list_for_each(p, &servers) {
		s = list_entry(p, ohc_server_t, snode);
		server_listen_close(s);
	}
}

int server_clear(unsigned short port)
{
	ohc_server_t *s = server_by_port(port);
	if(s == NULL) {
		log_error_admin(0, "no matched server");
		return OHC_ERROR;
	}

	s->clear++;
	return OHC_OK;
}

static inline struct list_head *server_lru_head(ohc_server_t *s)
{
	return s->capacity ? &s->lru_head : &shared_lru_head;
}

/* @format module call this to add an item, when load an item from device */
int server_load_fm_item(ohc_server_t *s, ohc_device_t *device,
		ohc_format_item_t *fm_item)
{
	ohc_item_t *item;
	size_t block_size;

	item = slab_alloc(&item_slab);
	if(item == NULL) {
		return OHC_ERROR;
	}

	item->putting = 0;
	item->badblock = 0;
	item->deleted = 0;
	item->used = 0;
	item->clear = 0;
	item->server_index = s->index;
	item->length = fm_item->length;
	item->expire = fm_item->expire;
	item->headers_len = fm_item->headers_len;
	item->offset = fm_item->offset;
	item->device_index = device->index;

	block_size = device_cut_free_block(item);
	if(block_size == 0) {
		slab_free(item);
		return OHC_ERROR;
	}

	memcpy(item->hnode.id, fm_item->hash_id, 16);
	hash_add(s->hash, &item->hnode, NULL, 0);
	list_add(&item->lru_node, server_lru_head(s));

	s->consumed += block_size;
	s->content += item->length;
	s->item_nr++;

	return OHC_OK;
}

/* pass-by item. only ID(uri), but no data(body). */
typedef struct {
	/* @hnode and @passby must be together, to
	 * distinguish ohc_item_t and ohc_passby_item_t. */
	ohc_hash_node_t		hnode;
	unsigned		passby:1;

	int32_t			expire;
	struct list_head	lru_node;
} ohc_passby_item_t;


static void server_passby_item_delete(ohc_server_t *s,
		ohc_passby_item_t *passby_item)
{
	hash_del(s->hash, &passby_item->hnode);
	list_del(&passby_item->lru_node);
	slab_free(passby_item);
	s->passby_item_nr--;
}

void server_item_delete(ohc_item_t *item)
{
	ohc_server_t *s = server_of_item(item);
	size_t block_size;

	/* 1st time get in here for the @item */
	if(item->deleted == 0) {
		hash_del(s->hash, &item->hnode);
	}

	/* if used, delete later */
	if(item->used != 0 || item->putting) {
		item->deleted = 1;
		return;
	}

	/* delete the item actally */
	list_del(&item->lru_node);
	s->content -= item->length;
	block_size = device_return_free_block(item);
	s->consumed -= block_size;
	s->item_nr--;
	slab_free(item);
}

inline int server_item_valid(ohc_item_t *item)
{
	return !device_of_item(item)->deleted
		&& !server_of_item(item)->deleted
		&& item->clear == server_of_item(item)->clear
		&& item->expire > timer_now(&master_timer);
}

static void server_item_expire(ohc_server_t *s, size_t target)
{
	ohc_item_t *item;
	ohc_passby_item_t *passby_item;
	struct list_head *p, *safe;
	time_t now = timer_now(&master_timer);
	size_t before = s->consumed;
	int count = 0;

	list_for_each_reverse_safe(p, safe, &s->lru_head) {
		item = list_entry(p, ohc_item_t, lru_node);

		if(before - s->consumed >= target && server_item_valid(item)) {
			break;
		}

		server_item_delete(item);

		if(count++ >= LOOP_LIMIT) {
			break;
		}
	}

	list_for_each_reverse_safe(p, safe, &s->passby_lru_head) {
		passby_item = list_entry(p, ohc_passby_item_t, lru_node);

		if(s->passby_enable && s->passby_item_nr < s->passby_limit_nr
				&& passby_item->expire > now) {
			break;
		}

		server_passby_item_delete(s, passby_item);

		if(count++ >= LOOP_LIMIT) {
			break;
		}
	}
}

static void server_shared_expire(size_t target)
{
	ohc_item_t *item;
	struct list_head *p, *safe;
	size_t size = 0;
	int count = 0;

	list_for_each_reverse_safe(p, safe, &shared_lru_head) {
		item = list_entry(p, ohc_item_t, lru_node);

		if(size >= target && server_item_valid(item)) {
			break;
		}

		size += item->length;
		server_item_delete(item);

		if(count++ >= LOOP_LIMIT) {
			break;
		}
	}
}

static ohc_hash_node_t *server_hash_get(ohc_request_t *r, unsigned char *hash_id)
{
	ohc_server_t *s = r->server;
	char key[REQ_BUF_SIZE]; /* REQ_BUF_SIZE is just enough */
	ssize_t length;

	/* key_include_query */
	if(s->key_include_query) {
		length = r->uri.len;
	} else {
		r->uri.base[r->uri.len] = '?';
		length = strchr(r->uri.base, '?') - r->uri.base;
	}
	length = http_decode_uri(r->uri.base, length, key);

	/* key_include_host */
	if(s->key_include_host && r->host.base) {
		memcpy(key + length, r->host.base, r->host.len);
		length += r->host.len;
	}

	/* key_include_ohc_key */
	if(s->key_include_ohc_key && r->ohc_key.base) {
		memcpy(key + length, r->ohc_key.base, r->ohc_key.len);
		length += r->ohc_key.len;
	}

	return hash_get(s->hash, (unsigned char *)key, length, hash_id);
}


/* request module call this, in a GET request, to get the item */
int server_request_get_handler(ohc_request_t *r)
{
	ohc_item_t *item;
	ohc_passby_item_t *passby_item;
	ohc_hash_node_t *hnode;
	ohc_server_t *s = r->server;

	s->gets++;
	s->gets_current_period++;

	hnode = server_hash_get(r, NULL);
	if(hnode == NULL) {
		return OHC_ERROR;
	}

	passby_item = list_entry(hnode, ohc_passby_item_t, hnode);
	if(passby_item->passby) {
		list_del(&passby_item->lru_node);
		list_add(&passby_item->lru_node, &s->passby_lru_head);
		s->passby_hits++;
		s->passby_hits_current_period++;
		return OHC_ERROR;
	}

	item = list_entry(hnode, ohc_item_t, hnode);
	if(item->deleted || item->putting) {
		return OHC_ERROR;
	}
	if(!server_item_valid(item)) {
		server_item_delete(item);
		return OHC_ERROR;
	}

	s->hits++;
	s->hits_current_period++;
	device_of_item(item)->used++;

	item->used++;
	r->item = item;

	/* update LRU */
	list_del(&item->lru_node);
	list_add(&item->lru_node, server_lru_head(s));

	return OHC_OK;
}

static int server_passby_store(ohc_server_t *s, unsigned char *hash_id)
{
	static ohc_slab_t passby_item_slab = OHC_SLAB_INIT(ohc_passby_item_t);

	ohc_passby_item_t *passby_item;

	if(!s->passby_enable || s->item_nr < s->passby_begin_item_nr
			     || s->consumed < s->passby_begin_consumed) {
		return OHC_DECLINE;
	}

	passby_item = slab_alloc(&passby_item_slab);
	if(passby_item == NULL) {
		log_error_run(0, "NoMem");
		return OHC_ERROR;
	}

	passby_item->passby = 1;
	passby_item->expire = timer_now(&master_timer) + s->passby_expire;
	memcpy(passby_item->hnode.id, hash_id, 16);
	hash_add(s->hash, &passby_item->hnode, NULL, 0);
	list_add(&passby_item->lru_node, &s->passby_lru_head);
	s->passby_item_nr++;
	s->passby_stores++;
	s->passby_stores_current_period++;

	return OHC_OK;
}

/* @request module call this, in a PUT request, to put an item */
int server_request_put_handler(ohc_request_t *r)
{
	ohc_item_t *item;
	ohc_passby_item_t *passby_item;
	ohc_hash_node_t *hnode;
	ohc_server_t *s;
	unsigned char hash_id[16];
	size_t block_size;
	time_t now;
	int try = 0;

	s = r->server;
	s->puts++;
	s->puts_current_period++;

	/* check size */
	if(s->item_max_size != 0 && r->content_length > s->item_max_size) {
		r->error_reason = "TooBigItem1";
		return OHC_DECLINE;
	}
	if(s->capacity != 0 && r->content_length + r->put_header_length > s->capacity) {
		r->error_reason = "TooBigItem2";
		return OHC_DECLINE;
	}

	/* check expire */
	now = timer_now(&master_timer);
	if(s->expire_force != 0) {
		r->expire = s->expire_force + now;

	} else if(r->expire == 0) {
		if(s->expire_default == 0) {
			r->error_reason = "Expired";
			return OHC_DECLINE;
		}
		r->expire = s->expire_default + now;

	} else {}

	/* check exist */
	hnode = server_hash_get(r, hash_id);
	if(hnode == NULL) {
		if(server_passby_store(s, hash_id) == OHC_OK) {
			r->error_reason = "StorePassby";
			return OHC_DECLINE;
		}
	} else {
		passby_item = list_entry(hnode, ohc_passby_item_t, hnode);
		item = list_entry(hnode, ohc_item_t, hnode);
		if(passby_item->passby) {
			server_passby_item_delete(s, passby_item);

		} else if(r->method == OHC_HTTP_METHOD_PUT || !server_item_valid(item)) {
			server_item_delete(item);

		} else {
			r->error_reason = "Exist";
			return OHC_DECLINE;
		}
	}

	/* check done, store the item now */

	item = slab_alloc(&item_slab);
	if(item == NULL) {
		log_error_run(0, "NoMem");
		return OHC_ERROR;
	}
	item->length = r->content_length + r->put_header_length;
	item->headers_len = r->put_header_length;

try_again:
	block_size = device_get_free_block(item);
	if(block_size == 0) {
		/* If fails in getting free block, expire some items and try again.
		 * The following expire order is complicated, and there is no
		 * specific reason for the order. Just feeling. */
		if(try++ < 2 && !list_empty(&s->lru_head)
				&& s->consumed + item->length*2 > s->capacity) {
			server_item_expire(s, item->length * 2);
			goto try_again;
		}
		if(try++ < 5 && !list_empty(&shared_lru_head)) {
			server_shared_expire(item->length * 2);
			goto try_again;
		}
		if(try++ < 9 && !list_empty(&s->lru_head)) {
			server_item_expire(s, item->length * 2);
			goto try_again;
		}
		if(try++ < 12) {
			device_free_block_extend(item->length);
			goto try_again;
		}

		slab_free(item);
		r->error_reason = "NoSpace";
		log_error_run(0, "space(%ld) alloc fail in server %d",
				item->length, s->listen_port);
		return OHC_ERROR;
	}

	/* done. update something */
	r->item = item;
	item->putting = 1;
	item->badblock = 0;
	item->deleted = 0;
	item->used = 0;
	item->clear = s->clear;
	item->expire = r->expire;
	item->server_index = s->index;
	memcpy(item->hnode.id, hash_id, 16);
	hash_add(s->hash, &item->hnode, NULL, 0);
	list_add(&item->lru_node, server_lru_head(s));
	s->consumed += block_size;
	s->content += item->length;
	s->item_nr++;
	s->stores++;
	s->stores_current_period++;
	device_of_item(item)->used++;

	return OHC_OK;
}

/* @request module call this, in a DELETE request, to delete an item */
int server_request_delete_handler(ohc_request_t *r)
{
	ohc_hash_node_t *hnode;
	ohc_item_t *item;
	ohc_passby_item_t *passby_item;
	ohc_server_t *s = r->server;

	s->deletes++;
	s->deletes_current_period++;

	hnode = server_hash_get(r, NULL);
	if(hnode == NULL) {
		return OHC_ERROR;
	}

	passby_item = list_entry(hnode, ohc_passby_item_t, hnode);
	if(passby_item->passby) {
		server_passby_item_delete(s, passby_item);
	} else {
		item = list_entry(hnode, ohc_item_t, hnode);
		server_item_delete(item);
	}
	return OHC_OK;
}

/* @request module call this, when a request finishs */
void server_request_finalize(ohc_request_t *r)
{
	ohc_item_t *item = r->item;
	int not_finish = 0;

	if(item == NULL) {
		return;
	}
	r->item = NULL;

	device_of_item(item)->used--;

	if(item->putting) {
		item->putting = 0;

		if(r->process_size < item->length) {
			not_finish = 1;
		}

	} else {
		item->used--;
	}

	if(r->disk_error) {
		item->badblock = 1;
		server_item_delete(item);

	} else if(item->deleted || not_finish) {
		server_item_delete(item);
	}
}

/* server port listen handler, called on new request */
void server_listen_handler(ohc_server_t *s)
{
	int sock_fd;
	struct sockaddr_in client;

	while(1) {
		sock_fd = tcp_accept(s->listen_fd, &client);
		if(sock_fd == -1) {
			if(errno == EAGAIN) {
				return;
			}
			log_error_run(errno, "accept in server %d", s->listen_port);
			return;
		}

		request_process_entry(s, sock_fd, &client);
	}
}

static void server_destroy(ohc_server_t *s)
{
	/* If s->capacity==0, server_item_expire() does not works, because
	 * the items are linked on shared_lru_head.
	 * So maybe we need hash_pop()? */
	server_item_expire(s, s->consumed);

	if(s->item_nr != 0 || s->passby_item_nr != 0) {
		return;
	}

	list_del(&s->snode);
	hash_destroy(s->hash);
	fclose(s->access_filp);
	idx_pointer_delete(&server_indexs, s->index);
	free(s);
}

/* regular routine, called by master thread */
void server_routine(void)
{
	struct list_head *p, *safep;
	ohc_server_t *s;
	time_t now;

	now = timer_now(&master_timer);
	list_for_each(p, &servers) {
		s = list_entry(p, ohc_server_t, snode);

		/* update statistics */
		if(now - s->last_clear >= s->status_period) {
			s->last_clear = now;

			s->gets_last_period = s->gets_current_period;
			s->hits_last_period = s->hits_current_period;
			s->passby_hits_last_period = s->passby_hits_current_period;
			s->gets_current_period = 0;
			s->hits_current_period = 0;
			s->passby_hits_current_period = 0;

			s->puts_last_period = s->puts_current_period;
			s->stores_last_period = s->stores_current_period;
			s->passby_stores_last_period = s->passby_stores_current_period;
			s->puts_current_period = 0;
			s->stores_current_period = 0;
			s->passby_stores_current_period = 0;

			s->deletes_last_period = s->deletes_current_period;
			s->deletes_current_period = 0;

			s->output_size_last_period = s->output_size_current_period;
			s->input_size_last_period = s->input_size_current_period;
			s->output_size_current_period = 0;
			s->input_size_current_period = 0;
		}

		/* expire item if over-size */
		server_item_expire(s, s->consumed > s->capacity ? s->consumed - s->capacity : 0);

		fflush(s->access_filp);
	}

	server_shared_expire(0);

	/* clear deleted servers */
	list_for_each_safe(p, safep, &deleted_servers) {
		s = list_entry(p, ohc_server_t, snode);
		server_destroy(s);
	}
}

void server_status(FILE *filp)
{
	struct list_head *p;
	ohc_server_t *s;

	fputs("\n- listen capacity period "
			"| consumed content items passbyitems connections "
			"| gets _gets hits _hits passbyhits _passbyhits "
			"| puts _puts stores _stores passbystores _passbystores "
			"| deletes _deletes "
			"| output input\n", filp);

	list_for_each(p, &servers) {
		s = list_entry(p, ohc_server_t, snode);
		fprintf(filp, "-- %d %ld %ld "
				"| %ld %ld %ld %ld %d "
				"| %ld %ld %ld %ld %ld %ld "
				"| %ld %ld %ld %ld %ld %ld "
				"| %ld %ld "
				"| %ld %ld\n",
				s->listen_port, s->capacity, s->status_period,
				s->consumed, s->content, s->item_nr, s->passby_item_nr, s->connections,
				s->gets, s->gets_last_period, s->hits, s->hits_last_period,
				s->passby_hits, s->passby_hits_last_period,
				s->puts, s->puts_last_period, s->stores, s->stores_last_period,
				s->passby_stores, s->passby_stores_last_period,
				s->deletes, s->deletes_last_period,
				s->output_size_last_period, s->input_size_last_period);
	}
}
