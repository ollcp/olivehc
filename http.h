/*
 * Parse HTTP request, make HTTP response.
 *
 * Author: Wu Bingzheng
 *
 */

#ifndef _OHC_HTTP_H_
#define _OHC_HTTP_H_

#include "olivehc.h"
#include "request.h"

enum http_methods {
	OHC_HTTP_METHOD_GET,
	OHC_HTTP_METHOD_HEAD,
	OHC_HTTP_METHOD_PUT,
	OHC_HTTP_METHOD_POST,
	OHC_HTTP_METHOD_PURGE,
	OHC_HTTP_METHOD_DELETE,
	OHC_HTTP_METHOD_INVALID,
};

struct http_method_s {
	string_t	str;
	void		*data;
};
extern struct http_method_s http_methods[];

#define RANGE_NO_SET	-1

int http_request_parse(ohc_request_t *r);
string_t *http_code_page(int code);
ssize_t http_decode_uri(const char *uri, ssize_t len, char *output);
ssize_t http_make_200_response_header(ssize_t content_length, char *output);
ssize_t http_make_206_response_header(ssize_t range_start, ssize_t range_end,
		ssize_t body_len, char *output);

#endif
