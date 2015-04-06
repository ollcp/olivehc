/**
 * Timer and time management.
 *
 * Auther: Wu Bingzheng
 *
 **/

#ifndef _OHC_TIMER_H_
#define _OHC_TIMER_H_

#include <time.h>
#include "list.h"

#define LEN_TIME_FARMAT_RFC1123	sizeof("Sun, 06 Nov 1994 08:49:23 GMT")
#define LEN_TIME_FARMAT_LOG	sizeof("1994-11-06 08:49:23")

#define BIG_TIME 3638880000

typedef struct {
	struct list_head	tgroup_head;
	struct list_head	expires;

	time_t		now;
	char		format_rfc1123[LEN_TIME_FARMAT_RFC1123];
	char		format_log[LEN_TIME_FARMAT_LOG];
} ohc_timer_t;

typedef struct ohc_timer_group_s ohc_timer_group_t;
typedef struct {
	struct list_head	tnode_node;
	time_t			timeout;
	ohc_timer_group_t	*group;
} ohc_timer_node_t;

time_t timer_parse_rfc1123(char *p);
void timer_destroy(ohc_timer_t *timer);
time_t timer_closest(ohc_timer_t *timer);
struct list_head *timer_expire(ohc_timer_t *timer);
int timer_add(ohc_timer_t *timer, ohc_timer_node_t *tnode, time_t timeout);
void timer_del(ohc_timer_node_t *tnode);
int timer_update(ohc_timer_node_t *tnode, time_t timeout);
void timer_init(ohc_timer_t *timer);
char *timer_format_rfc1123(ohc_timer_t *timer);
char *timer_format_log(ohc_timer_t *timer);

static inline time_t timer_now(ohc_timer_t *timer)
{
	return timer->now;
}

static inline time_t timer_refresh(ohc_timer_t *timer)
{
	timer->now = time(NULL);
	timer->format_rfc1123[0] = '\0';
	timer->format_log[0] = '\0';

	return timer->now;
}

#endif
