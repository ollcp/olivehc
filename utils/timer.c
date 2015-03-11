/**
 *
 * Timer and time management.
 *
 * Auther: Wu Bingzheng
 *
 **/

#include "timer.h"
#include "list.h"
#include "slab.h"
#include <string.h>
#include <stdlib.h>

struct ohc_timer_group_s {
	struct list_head	tnode_head;
	struct list_head	tgroup_node;

	time_t			timeout;
	ohc_timer_t		*timer;
};

void timer_init(ohc_timer_t *timer)
{
	INIT_LIST_HEAD(&timer->tgroup_head);
	INIT_LIST_HEAD(&timer->expires);
	timer_refresh(timer);
}

static char *month_str[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static char *week_str[] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/* parse RFC1123 time string */
time_t timer_parse_rfc1123(char *p)
{
	struct tm t;
	int i;
	time_t ret;
#define D2(p) ((*(p)-'0')*10 + *((p)+1)-'0')
#define INVALID_TIME (time_t)-1

	if(strncmp(p + 25, " GMT", 4)) {
		return INVALID_TIME;
	}
	t.tm_mday = D2(p + 5);
	t.tm_year = D2(p + 12) * 100 + D2(p + 14) - 1900;
	t.tm_hour = D2(p + 17);
	t.tm_min  = D2(p + 20);
	t.tm_sec  = D2(p + 23);
	for(i = 0; i < 12; i++) {
		if(strncmp(p + 8, month_str[i], 3) == 0) {
			break;
		}
	}
	if(i == 12) {
		return INVALID_TIME;
	}
	t.tm_mon = i;
	ret = mktime(&t);
	if(ret == INVALID_TIME) {
		return INVALID_TIME;
	}
	return ret + t.tm_gmtoff;
}

/* return RFC1123 time string */
char *timer_format_rfc1123(ohc_timer_t *timer)
{
	struct tm tm;
	if(timer->format_rfc1123[0] == '\0') {
		gmtime_r(&timer->now, &tm);
		snprintf(timer->format_rfc1123, LEN_TIME_FARMAT_RFC1123,
				"%s, %02d %s %d %02d:%02d:%02d GMT",
				week_str[tm.tm_wday], tm.tm_mday, month_str[tm.tm_mon],
				tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
	}

	return timer->format_rfc1123;
}

/* return human readable time string */
char *timer_format_log(ohc_timer_t *timer)
{
	struct tm tm;
	if(timer->format_log[0] == '\0') {
		localtime_r(&timer->now, &tm);
		snprintf(timer->format_log, LEN_TIME_FARMAT_LOG,
				"%d-%02d-%02d %02d:%02d:%02d",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
				tm.tm_hour, tm.tm_min, tm.tm_sec);
	}

	return timer->format_log;
}

/* return the closest timer's timeout (return a big time if no timer) */
time_t timer_closest(ohc_timer_t *timer)
{
	time_t closest = BIG_TIME;
	struct list_head *p, *safe;
	ohc_timer_group_t *tgroup;
	ohc_timer_node_t *tnode;

	list_for_each_safe(p, safe, &timer->tgroup_head) {

		tgroup = list_entry(p, ohc_timer_group_t, tgroup_node);
		if(list_empty(&tgroup->tnode_head)) {
			list_del(&tgroup->tgroup_node);
			free(tgroup);
			continue;
		}

		tnode = list_entry(tgroup->tnode_head.next,
				ohc_timer_node_t, tnode_node);
		if(tnode->timeout < closest) {
			closest = tnode->timeout;
		}
	}

	timer_refresh(timer);
	return (closest <= timer->now) ? 0 : closest - timer->now;
}

/* find expired nodes, delete them, and return them by a list. */
struct list_head *timer_expire(ohc_timer_t *timer)
{
	struct list_head *p, *q, *safeq;
	ohc_timer_group_t *tgroup;
	ohc_timer_node_t *tnode;

	timer_refresh(timer);
	list_for_each(p, &timer->tgroup_head) {
		tgroup = list_entry(p, ohc_timer_group_t, tgroup_node);
		list_for_each_safe(q, safeq, &tgroup->tnode_head) {

			tnode = list_entry(q, ohc_timer_node_t, tnode_node);
			if(tnode->timeout > timer->now) {
				break;
			}

			list_del(q);
			list_add_tail(q, &timer->expires);
		}
	}

	return &timer->expires;
}

/* add a timer */
int timer_add(ohc_timer_t *timer, ohc_timer_node_t *tnode, time_t timeout)
{
	struct list_head *p;
	ohc_timer_group_t *tgroup;

	/* search for the timer_group with @timeout */
	list_for_each(p, &timer->tgroup_head) {
		tgroup = list_entry(p, ohc_timer_group_t, tgroup_node);
		if(tgroup->timeout == timeout) {
			break;
		}
	}

	/* not found, create one */
	if(p == &timer->tgroup_head) {
		tgroup = (ohc_timer_group_t *)malloc(sizeof(ohc_timer_group_t));
		if(tgroup == NULL) {
			return 1;
		}

		tgroup->timeout = timeout;
		tgroup->timer = timer;
		INIT_LIST_HEAD(&tgroup->tnode_head);
		list_add(&tgroup->tgroup_node, &timer->tgroup_head);
	}

	/* add timer */
	list_add_tail(&tnode->tnode_node, &tgroup->tnode_head);
	tnode->timeout = timer->now + timeout;
	tnode->group = tgroup;

	return 0;
}

/* delete a timer */
void timer_del(ohc_timer_node_t *tnode)
{
	list_del(&tnode->tnode_node);
}

/* update a timer with new timeout (if @timeout==0, use previous timeout) */
int timer_update(ohc_timer_node_t *tnode, time_t timeout)
{
	ohc_timer_group_t *tgroup = tnode->group;

	if(timeout == 0 || timeout == tgroup->timeout) {
		tnode->timeout = tgroup->timer->now + tgroup->timeout;
		list_del(&tnode->tnode_node);
		list_add_tail(&tnode->tnode_node, &tgroup->tnode_head);
		return 0;
	} else {
		timer_del(tnode);
		return timer_add(tgroup->timer, tnode, timeout);
	}
}

/* destroy */
void timer_destroy(ohc_timer_t *timer)
{
	struct list_head *p;
	list_for_each(p, &timer->tgroup_head) {
		free(list_entry(p, ohc_timer_group_t, tgroup_node));
	}
}
