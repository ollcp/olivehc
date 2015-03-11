/*
 * Utils for request event management.
 *
 * Author: Wu Bingzheng
 *
 */

#include "event.h"

#define OHC_EV_READ  1
#define OHC_EV_WRITE 2

static int event_add(ohc_request_t *r, req_handler_f *handler, int event, time_t timeout)
{
	uint32_t epoll_ev = (event == OHC_EV_READ) ? EPOLLIN : EPOLLOUT;
	int epoll_fd = r->worker_thread ? r->worker_thread->epoll_fd : master_epoll_fd;
	ohc_timer_t *timer;

	if(r->events == event) {
		timer_update(&r->tnode, timeout);

	} else if(r->events) {
		if(epoll_mod(epoll_fd, r->sock_fd, epoll_ev, r) != 0) {
			return OHC_ERROR;
		}
		if(timer_update(&r->tnode, timeout) != 0) {
			return OHC_ERROR;
		}
	} else {
		timer = r->worker_thread ? &r->worker_thread->timer : &master_timer;
		if(epoll_add(epoll_fd, r->sock_fd, epoll_ev, r) != 0) {
			return OHC_ERROR;
		}
		if(timer_add(timer, &r->tnode, timeout) != 0) {
			return OHC_ERROR;
		}
	}

	r->events = event;
	r->event_handler = handler;
	return OHC_OK;
}

int event_add_read(ohc_request_t *r, req_handler_f *handler)
{
	return event_add(r, handler, OHC_EV_READ, r->server->recv_timeout);
}

int event_add_write(ohc_request_t *r, req_handler_f *handler)
{
	return event_add(r, handler, OHC_EV_WRITE, r->server->send_timeout);
}

int event_add_keepalive(ohc_request_t *r, req_handler_f *handler)
{
	return event_add(r, handler, OHC_EV_READ, r->server->keepalive_timeout);
}

void event_del(ohc_request_t *r)
{
	if(r->events) {
		int epoll_fd = r->worker_thread ? r->worker_thread->epoll_fd : master_epoll_fd;
		epoll_del(epoll_fd, r->sock_fd);

		timer_del(&r->tnode);

		r->events = 0;
	}
}
