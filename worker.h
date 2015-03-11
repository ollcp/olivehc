/*
 * Worker thread management, and communication between master.
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OHC_WORKER_H_
#define _OHC_WORKER_H_ 

#include "olivehc.h"

struct ohc_worker_s {
	struct list_head	wnode;
	struct list_head	working_requests;
	struct list_head	blocked_requests;
	ohc_timer_t		timer;

	time_t		quit_time;
	pthread_t	tid;
	int		epoll_fd;

	/* only master update this, and worker check this. */
	int		request_nr;

	/**
	 * two pipes.
	 * 1. master write request into @dispatch_fd, worker read
	 *    @receive_fd to receive request;
	 * 2. worker write the finished requests into @return_fd,
	 *    and master receive the requests from @receive_fd.
	 **/
	int		recycle_fd;
	int		dispatch_fd;
	int		receive_fd;
	int		return_fd;

};

int worker_conf_check(ohc_conf_t *conf_cycle);
void worker_conf_load(ohc_conf_t *conf_cycle);
void worker_conf_rollback(ohc_conf_t *conf_cycle);

void worker_quit(time_t quit_time);

/* master calls */
int worker_request_dispatch(ohc_request_t *r, req_handler_f *handler);
void worker_request_recycle(ohc_worker_t *worker);

/* worker calls */
int worker_request_return(ohc_request_t *r, req_handler_f *handler);
void worker_request_receive(ohc_worker_t *worker);

#endif
