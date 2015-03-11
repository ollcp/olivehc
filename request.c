/*
 * Flow of request. Read request, write response.
 *
 * Author: Wu Bingzheng
 *
 */

#define _XOPEN_SOURCE 500 /* for pwrite */
#include "request.h"

static int connections_total = 0;

static void request_read_request_header(ohc_request_t *r);

static inline void request_cork_set(ohc_request_t *r)
{
	set_cork(r->sock_fd, 1);
	r->cork = 1;
}

static inline void request_cork_clear(ohc_request_t *r)
{
	if(r->cork) {
		set_cork(r->sock_fd, 0);
		r->cork = 0;
	}
}

static void request_reset(ohc_request_t *r)
{
	r->item = NULL;
	r->worker_thread = NULL;
	r->events = 0;
	r->keepalive = r->server->keepalive_timeout ? 1 : 0;
	r->active = 0;
	r->connection_broken = 0;
	r->cork = 0;
	r->range_set = 0;
	r->disk_error = 0;
	r->output_size = 0;
	r->input_size = 0;
	r->event_handler = NULL;
	r->method = OHC_HTTP_METHOD_INVALID;
	r->uri.base = NULL;
	r->host.base = NULL;
	r->ohc_key.base = NULL;
	r->range.base = NULL;
	r->content_length = -1;
	r->http_code = 0;
	r->expire = 0;
	r->start_time = timer_now(&master_timer);
	r->error_reason = NULL;
	r->error_number = 0;
	r->buf_pos = r->_buffer;
	r->process_size = 0;

	/* other members will be set later */
}

/* Send a short buffer at the beginning of response. so we assume
 * that the socket sndbuf is empty and there is no block.
 * If blocked, in case, we do not record the breakpoint, and
 * return OHC_ERROR (while not OHC_AGAIN). */
static int request_send_buffer(ohc_request_t *r, char *buffer, ssize_t length)
{
	ssize_t rc;

interupted:
	rc = send(r->sock_fd, buffer, length, 0);
	if(rc != length) {
		if(rc < 0 && errno == EINTR) {
			goto interupted;
		}
		log_error_run(0, "request_send_buffer() blocks: %ld %ld", length, rc);
		r->error_reason = "SendError";
		r->error_number = errno;
		r->connection_broken = 1;
		return OHC_ERROR;
	}

	r->output_size += rc;
	return OHC_OK;
}

static int request_send_file(ohc_request_t *r, off_t start, off_t length)
{
	ssize_t rc;
	off_t off;
	ohc_device_t *device = device_of_item(r->item);

	off = start + r->process_size;
interupted:
	rc = sendfile(r->sock_fd, device->fd, &off, length - r->process_size);
	if(rc == -1) {
		if(errno == EAGAIN) {
			return OHC_AGAIN;
		}
		if(errno == EINTR) {
			goto interupted;
		}

		if(errno == EIO) {
			r->disk_error = 1;
			log_error_run(errno, "sendfile server:%d, "
					"device:%s, off:%ld, len:%ld",
					r->server->listen_port, device->filename,
					off, length - r->process_size);
		} else {
			r->connection_broken = 1;
		}
		r->error_reason = "SendfileError";
		r->error_number = errno;
		return OHC_ERROR;
	}

	r->output_size += rc;
	r->process_size += rc;
	if(length != r->process_size) {
		return OHC_AGAIN;
	}

	return OHC_OK;
}


static int request_write_disk(ohc_request_t *r, char *buffer, off_t length)
{
	ohc_item_t *item = r->item;
	ohc_device_t *device;
	int rc;

	/* item may be NULL, if we are not going to store the item,
	 * such as the item is too big, or store it as passby. */
	if(item == NULL) {
		goto out;
	}

	device = device_of_item(item);
	rc = pwrite(device->fd, buffer, length, item->offset + r->process_size);
	if(rc != length) {
		log_error_run(errno, "pwrite, server:%d, device:%s, "
				"off:%ld, len:%ld, ret:%ld",
				r->server->listen_port, device->filename,
				item->offset + r->process_size, length, rc);
		r->http_code = 500;
		r->disk_error = 1;
		r->error_reason = "WriteDiskError";
		r->error_number = errno;
		return OHC_ERROR;
	}

out:
	r->process_size += length;
	return OHC_OK;
}

static void request_do_finalize(ohc_request_t *r)
{
	ohc_server_t *s = r->server;
	FILE *fp = s->access_filp;

	server_request_finalize(r);
	event_del(r);
	s->output_size_current_period += r->output_size;
	s->input_size_current_period += r->input_size;

	/* log */
	if(r->active) {
		
		fprintf(fp, "%s %s %d %ld %s%s %s %s %s",
			timer_format_log(&master_timer),
			inet_ntoa(r->client.sin_addr),
			r->http_code,
			timer_now(&master_timer) - r->start_time,
			http_methods[r->method].str.base,
			strshow(&r->uri), strshow(&r->host),
			strshow(&r->ohc_key), strshow(&r->range));

		if(r->error_reason) {
			if(r->http_code == 204) {
				fprintf(fp, " [%s]\n", r->error_reason);

			} else if(r->error_number) {
				fprintf(fp, " [%s(%s) while %s]\n", r->error_reason,
					strerror(r->error_number), r->step);
			} else {
				fprintf(fp, " [%s while %s]\n",
					r->error_reason, r->step);
			}
		} else {
			fputs("\n", fp);
		}
	}

	if(r->keepalive && !r->connection_broken) {
		request_reset(r);
		event_add_keepalive(r, request_read_request_header);
		return;
	}

	list_del(&r->rnode);
	s->connections--;
	connections_total--;
	close(r->sock_fd);
	slab_free(r);
}

static void request_finalize(ohc_request_t *r)
{
	if(!r->connection_broken && r->output_size == 0) {
		/* don't check @request_send_buffer's return, for simple.*/
		string_t *page = http_code_page(r->http_code);
		request_send_buffer(r, page->base, page->len);
	}

	if(r->worker_thread) {
		worker_request_return(r, request_do_finalize);
	} else {
		request_do_finalize(r);
	}
}

static void request_put_read_request_body(ohc_request_t *r)
{
	ssize_t rc;
#define RECV_BUF_SIZE (100*1024)
	char buf[RECV_BUF_SIZE];
	size_t item_len = r->content_length + r->put_header_length;

	r->step = "ReadBody";

	/* receive from socket, and write into disk file */
	while(r->process_size < item_len) {

		/* receive */
		rc = recv(r->sock_fd, buf, RECV_BUF_SIZE, 0);
		if(rc == -1) {
			if(errno == EAGAIN) {
				goto again;
			} else if(errno == EINTR) {
				continue;
			} else {
				r->error_reason = "ReceiveError";
				r->error_number = errno;
				r->connection_broken = 1;
				goto finish;
			}
		}
		if(rc == 0) {
			r->error_reason = "ClientClose";
			r->connection_broken = 1;
			goto finish;
		}
		r->input_size += rc;
		if(rc > item_len - r->process_size) {
			r->error_reason = "BodyLargerThanDeclared";
			r->http_code = 400;
			r->keepalive = 0;
			goto finish;
		}

		/* write */
		rc = request_write_disk(r, buf, rc);
		if(rc == OHC_ERROR) {
			goto finish;
		}
	}

finish:
	request_finalize(r);
	return;

again:
	event_add_read(r, request_put_read_request_body);
	return;
}


static void request_put_read_request_body_preread(ohc_request_t *r)
{
	ssize_t len;
	ssize_t rc;
	char buffer[REQ_BUF_SIZE];
	int i;
	string_t *s;

	r->step = "PreReadBody";

	len = http_make_200_response_header(r->content_length, buffer);
	for(i = 0; i < r->put_header_nr; i++) {
		s = &r->put_headers[i];
		memcpy(buffer + len, s->base, s->len);
		len += s->len;
	}

	rc = request_write_disk(r, buffer, len);
	if(rc == OHC_ERROR) {
		request_finalize(r);
		return;
	}

	request_put_read_request_body(r);
}

static void request_get_write_response(ohc_request_t *r)
{
	int rc;

	r->step = "WriteResponse";

	rc = request_send_file(r, r->item->offset,
			(r->method == OHC_HTTP_METHOD_HEAD)
			? r->item->headers_len : r->item->length);

	if(rc == OHC_AGAIN) {
		event_add_write(r, request_get_write_response);
	} else { /* rc == OHC_OK || rc == OHC_ERROR */
		request_finalize(r);
	}
}

static void request_get_write_response_206_body(ohc_request_t *r)
{
	int rc;

	r->step = "WriteBody";

	rc = request_send_file(r, r->item->offset + r->item->headers_len + r->range_start,
			r->range_end - r->range_start + 1);

	request_cork_clear(r);

	if(rc == OHC_AGAIN) {
		event_add_write(r, request_get_write_response_206_body);
	} else { /* rc == OHC_OK || rc == OHC_ERROR */
		request_finalize(r);
	}
}

static void request_get_write_response_206_header_disk(ohc_request_t *r)
{
	ohc_item_t *item = r->item;
	ssize_t body_len = item->length - item->headers_len;
	ssize_t off = http_make_200_response_header(body_len, NULL);
	int rc;

	r->step = "WriteHeaderDisk";

	rc = request_send_file(r, item->offset + off, item->headers_len - off);

	if(rc == OHC_AGAIN) {
		request_cork_clear(r);
		event_add_write(r, request_get_write_response_206_header_disk);
		return;
	}
	if(rc == OHC_ERROR) {
		request_finalize(r);
		return;
	}

	/* rc == OHC_OK */
	if(r->method == OHC_HTTP_METHOD_HEAD) {
		request_cork_clear(r);
		request_finalize(r);
	} else {
		/* reset @process_size, because there is a new round of
		 * @request_send_file later. */
		r->process_size = 0;
		request_get_write_response_206_body(r);
	}
}

static void request_get_write_response_206_header_mem(ohc_request_t *r)
{
	int rc;
	char buffer[1000];
	ssize_t length;
	ssize_t body_len = r->item->length - r->item->headers_len;

	r->step = "WriteHeaderMem";

	if(r->range_start == RANGE_NO_SET) {
		r->range_start = r->range_end > body_len
			? 0 : body_len - r->range_end;
		r->range_end = body_len - 1;
	} else if(r->range_end == RANGE_NO_SET) {
		r->range_end = body_len - 1;
	} else if(r->range_end >= body_len) {
		r->range_end = body_len - 1;
	} else {}

	if(r->range_start >= body_len) {
		r->http_code = 416;
		request_finalize(r);
		return;
	}

	request_cork_set(r);

	length = http_make_206_response_header(r->range_start,
			r->range_end, body_len, buffer);
	rc = request_send_buffer(r, buffer, length);
	if(rc != OHC_OK) {
		request_finalize(r);
		return;
	}

	request_get_write_response_206_header_disk(r);
}

static void request_read_request_header(ohc_request_t *r)
{
	int rc;

	r->step = "ReadHeader";

	/* receive */
interupted:
	rc = recv(r->sock_fd, r->buf_pos,
			r->_buffer + REQ_BUF_SIZE - r->buf_pos - 1, 0);
	if(rc == -1) {
		if(errno == EAGAIN) {
			goto again;
		} else if(errno == EINTR) {
			goto interupted;
		} else {
			r->connection_broken = 1;
			r->error_reason = "ReceiveError";
			r->error_number = errno;
			goto fail;
		}
	}
	if(rc == 0) {
		r->error_reason = "ClientClose";
		r->connection_broken = 1;
		goto fail;
	}

	r->active = 1;
	r->buf_pos += rc;
	r->input_size += rc;

	/* http parse */
	rc = http_request_parse(r);
	if(rc == OHC_AGAIN) {

		/* request is too huge */
		if(r->buf_pos - r->_buffer >= REQ_BUF_SIZE - 1) {
			r->error_reason = "RequestHeadTooBig";
			r->http_code = 400;
			goto fail;
		}

		goto again;
	}
	if(rc == OHC_ERROR) {
		goto fail;
	}

	/* rc == OHC_DONE, parse ok */

	switch(r->method) {
	case OHC_HTTP_METHOD_GET:
	case OHC_HTTP_METHOD_HEAD:
		rc = server_request_get_handler(r);
		if(rc == OHC_ERROR) {
			r->http_code = 404;
			goto fail;
		}

		if(r->range_set) {
			r->http_code = 206;
			rc = worker_request_dispatch(r, request_get_write_response_206_header_mem);
		} else {
			r->http_code = 200;
			rc = worker_request_dispatch(r, request_get_write_response);
		}
		if(rc == OHC_ERROR) {
			r->http_code = 500;
			r->error_reason = "TooBusy";
			r->error_number = errno;
			goto fail;
		}
		break;

	case OHC_HTTP_METHOD_PUT:
	case OHC_HTTP_METHOD_POST:
		rc = server_request_put_handler(r);
		if(rc == OHC_ERROR) {
			r->http_code = 500;
			goto fail;
		}
		if(rc == OHC_DECLINE) {
			r->http_code = 204;
			if(r->server->shutdown_if_not_store) {
				r->keepalive = 0;
				goto fail;
			}
		} else {
			r->http_code = 201;
		}

		rc = worker_request_dispatch(r, request_put_read_request_body_preread);
		if(rc == OHC_ERROR) {
			r->http_code = 500;
			r->error_reason = "TooBusy";
			r->error_number = errno;
			goto fail;
		}
		break;

	case OHC_HTTP_METHOD_PURGE:
	case OHC_HTTP_METHOD_DELETE:
		rc = server_request_delete_handler(r);
		if(rc == OHC_ERROR) {
			r->http_code = 404;
			goto fail;
		}
		r->http_code = 204;
		request_finalize(r);
		break;

	default:
		;
	}

	return;

again:
	event_add_read(r, request_read_request_header);
	return;
fail:
	request_finalize(r);
	return;
}

/*  entry of request process, called when receive a new request */
void request_process_entry(ohc_server_t *s, int sock_fd, struct sockaddr_in *client)
{
	static ohc_slab_t request_slab = OHC_SLAB_INIT(ohc_request_t);

	ohc_request_t *r;

	if(s->connections >= s->connections_limit) {
		log_error_run(0, "exceed connections limit in server %d", s->listen_port);
		close(sock_fd);
		return;
	}

	r = (ohc_request_t *)slab_alloc(&request_slab);
	if(r == NULL) {
		log_error_run(0, "NoMem");
		close(sock_fd);
		return;
	}

	list_add(&r->rnode, &master_requests);
	s->connections++;
	connections_total++;

	r->server = s;
	r->sock_fd = sock_fd;
	r->client = *client;

	request_reset(r);

	request_read_request_header(r);
}

void request_timeout_handler(ohc_request_t *r)
{
	r->connection_broken = 1;
	r->error_reason = "Timeout";

	request_finalize(r);
}

/* close requests linked on @requests.
 * if @keepalive_only is set, clean requests only in keepalive idle. */
void request_clean(struct list_head *requests, int keepalive_only)
{
	struct list_head *p, *safe;
	ohc_request_t *r;
	list_for_each_safe(p, safe, requests) {
		r = list_entry(p, ohc_request_t, rnode);
		if(keepalive_only && r->active) {
			continue;
		}

		if(r->event_handler != request_finalize) {
			r->connection_broken = 1;
			r->error_reason = "CleanByQuitTimeout";
		}
		request_finalize(r);
	}
}

/* check can we quit, if there is no request */
int request_check_quit(int keepalive_only)
{
	request_clean(&master_requests, keepalive_only);
	return connections_total == 0;
}
