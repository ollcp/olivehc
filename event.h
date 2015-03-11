/*
 * Utils for request event management.
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OHC_EVENT_H_
#define _OHC_EVENT_H_

#include "olivehc.h"

int event_add_read(ohc_request_t *r, req_handler_f *handler);
int event_add_write(ohc_request_t *r, req_handler_f *handler);
int event_add_keepalive(ohc_request_t *r, req_handler_f *handler);
void event_del(ohc_request_t *r);

#endif
