/*
 * Include and define all things.
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OLIVEHC_H_
#define _OLIVEHC_H_

#define OHC_VERSION	"OliveHC 1.2.2"

#define OHC_OK		0
#define OHC_ERROR	-1
#define OHC_DONE	-2
#define OHC_AGAIN	-3
#define OHC_DECLINE	-4

#define PATH_LENGTH	1024

#define LOOP_LIMIT	1000

#define EVENT_TYPE_SOCKET	0
#define EVENT_TYPE_LISTEN	1
#define EVENT_TYPE_PIPE		2
#define EVENT_TYPE_MASK		3UL

typedef char ohc_flag_t;

//daemon()
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stddef.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <arpa/inet.h>  
#include <linux/fs.h>
#include <netinet/tcp.h>


/* utils */
#include "utils/list.h"
#include "utils/socktcp.h"
#include "utils/string.h"
#include "utils/slab.h"
#include "utils/hash.h"
#include "utils/epoll.h"
#include "utils/timer.h"
#include "utils/ipbucket.h"
#include "utils/idx_pointer.h"


typedef struct ohc_request_s ohc_request_t;
typedef struct ohc_item_s ohc_item_t;
typedef struct ohc_server_s ohc_server_t;
typedef struct ohc_device_s ohc_device_t;
typedef struct ohc_worker_s ohc_worker_t;
typedef struct ohc_format_item_s ohc_format_item_t;
typedef struct ohc_conf_s ohc_conf_t;
typedef void req_handler_f(ohc_request_t *r);

extern int master_epoll_fd;
extern ohc_timer_t master_timer;
extern struct list_head master_requests;

#include "conf.h"
#include "format.h"
#include "http.h"
#include "server.h"
#include "worker.h"
#include "device.h"
#include "request.h"
#include "event.h"

void log_error(FILE *filp, const char *prefix, int errnum, const char *fmt, ...);

extern FILE *error_filp;
#define log_error_run(errnum, fmt, ...) \
	log_error(error_filp, timer_format_log(&master_timer), errnum, fmt, ##__VA_ARGS__)

extern FILE *admin_out_filp;
#define log_error_admin(errnum, fmt, ...) \
	log_error(admin_out_filp, "ERROR:", errnum, fmt, ##__VA_ARGS__)

#endif
