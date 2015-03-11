/*
 * Better interfaces for Epoll.
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OHC_EPOLL_H_
#define _OHC_EPOLL_H_

#include <sys/epoll.h>

static inline int epoll_add(int epoll_fd, int sock_fd, uint32_t event, void *data)
{
	struct epoll_event ev;
	ev.events = event;
	ev.data.ptr = data;
	return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ev);
}

static inline int epoll_add_read(int epoll_fd, int sock_fd, void *data)
{
	return epoll_add(epoll_fd, sock_fd, EPOLLIN, data);
}

static inline int epoll_add_write(int epoll_fd, int sock_fd, void *data)
{
	return epoll_add(epoll_fd, sock_fd, EPOLLOUT, data);
}

static inline int epoll_add_rdwr(int epoll_fd, int sock_fd, void *data)
{
	return epoll_add(epoll_fd, sock_fd, EPOLLIN | EPOLLOUT, data);
}

static inline int epoll_mod(int epoll_fd, int sock_fd, uint32_t event, void *data)
{
	struct epoll_event ev;
	ev.events = event;
	ev.data.ptr = data;
	return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, sock_fd, &ev);
}

static inline int epoll_mod_read(int epoll_fd, int sock_fd, void *data)
{
	return epoll_mod(epoll_fd, sock_fd, EPOLLIN, data);
}

static inline int epoll_mod_write(int epoll_fd, int sock_fd, void *data)
{
	return epoll_mod(epoll_fd, sock_fd, EPOLLOUT, data);
}

static inline int epoll_del(int epoll_fd, int sock_fd)
{
	return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sock_fd, 0);
}

#endif
