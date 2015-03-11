/*
 * Flow of request. Read request, write response.
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OHC_REQUEST_H_
#define _OHC_REQUEST_H_

#include "olivehc.h"

#define REQ_BUF_SIZE	4096
/* a request, include its downstream connection */
struct ohc_request_s {
	ohc_server_t	*server;

	ohc_item_t	*item;

	ohc_worker_t	*worker_thread;

	unsigned	events:2;
	unsigned	keepalive:1;
	unsigned	active:1;
	unsigned	connection_broken:1;
	unsigned	cork:1;
	unsigned	range_set:1;
	unsigned	disk_error:1;

	/* request line and headers */
	int		method;
	ssize_t		content_length;
	ssize_t		range_start;
	ssize_t		range_end;
	string_t	range;
	string_t	uri;
	string_t	host;
	string_t	ohc_key;
	string_t	put_headers[10]; /* at most #(http_request_header_put) */
	int		put_header_nr;
	int		put_header_length;
	time_t		expire;

	time_t		start_time;

	/* in GET, record sendfile process size;
	 * in PUT, record recv item process size. */
	size_t		process_size;

	size_t		output_size;
	size_t		input_size;

	int		http_code;
	int		error_number;
	char		*error_reason;
	char		*step;

	req_handler_f	*event_handler;

	char		*buf_pos;
	char		_buffer[REQ_BUF_SIZE];

	int			sock_fd;
	struct sockaddr_in	client;

	ohc_timer_node_t	tnode;
	struct list_head	rnode;
};

void request_process_entry(ohc_server_t *s, int sock_fd, struct sockaddr_in *client);
void request_timeout_handler(ohc_request_t *r);
void request_clean(struct list_head *requests, int keepalive_only);
int request_check_quit(int keepalive_only);

#endif
