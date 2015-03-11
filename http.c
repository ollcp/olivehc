/*
 * Parse HTTP request, make HTTP response.
 *
 * Author: Wu Bingzheng
 *
 */

#include "http.h"


typedef int http_parse_f(ohc_request_t *, char *, ssize_t);
struct http_header_s {
	string_t	name;
	http_parse_f	*handler;
};

static int http_parse_put_content_length(ohc_request_t *r, char *p, ssize_t len)
{
	char *endp;
	r->content_length = strtoul(p, &endp, 10);
	if(endp == p || endp != p + len) {
		r->error_reason = "InvalidContentLength";
		return OHC_ERROR;
	}
	return OHC_OK;
}

static int http_parse_host(ohc_request_t *r, char *p, ssize_t len)
{
	r->host.base = p;
	r->host.len = len;
	return OHC_OK;
}

static int http_parse_ohc_key(ohc_request_t *r, char *p, ssize_t len)
{
	r->ohc_key.base = p;
	r->ohc_key.len = len;
	return OHC_OK;
}

static int http_parse_put_cache_control(ohc_request_t *r, char *p, ssize_t len)
{
	char *value, *endp;

	if(r->server->expire_force != 0) {
		return OHC_DECLINE;
	}
	if(strncmp(p, "max-age=", 8) != 0) {
		r->error_reason = "UnhandledCacheControl";
		return OHC_ERROR;
	}

	value = p + 8;
	r->expire = strtoul(value, &endp, 10);
	if(endp == p || endp != p + len) {
		r->error_reason = "InvalidMaxAge";
		return OHC_ERROR;
	}
	if(r->expire == 0) {
		r->error_reason = "max-age=0";
		return OHC_ERROR;
	}
	r->expire += timer_now(&master_timer);

	return OHC_DECLINE;
}

static int http_parse_put_expires(ohc_request_t *r, char *p, ssize_t len)
{
	if(r->server->expire_force != 0 || r->expire != 0) {
		return OHC_DECLINE;
	}
	r->expire = timer_parse_rfc1123(p);
	if(r->expire == (time_t)-1 || r->expire <= timer_now(&master_timer)) {
		r->error_reason = "InvalidExpires";
		return OHC_ERROR;
	}
	return OHC_DECLINE;
}

static int http_parse_get_range(ohc_request_t *r, char *p, ssize_t len)
{
	ssize_t start, end;
	char *begin = p;
	char *q;

	r->range.base = begin;
	r->range.len = len;

	if(strncmp(p, "bytes=", 6)) {
		goto fail;
	}
	p += 6;

	/* start */
	q = p;
	start = 0;
	while (*p >= '0' && *p <= '9') {
		start = start * 10 + *p++ - '0';
	}
	if(p == q) {
		start = RANGE_NO_SET;
	}

	/* - */
	if(*p++ != '-') {
		goto fail;
	}

	/* end */
	q = p;
	end = 0;
	while (*p >= '0' && *p <= '9') {
		end = end * 10 + *p++ - '0';
	}
	if(p == q) {
		end = RANGE_NO_SET;
	}

	/* check */
	if(start == RANGE_NO_SET && end == RANGE_NO_SET) {
		goto fail;
	}
	if(start != RANGE_NO_SET && end != RANGE_NO_SET && start > end) {
		goto fail;
	}

	while(*p == ' ') p++;
	if(*p == ',') {
		/* not surport multi-range */
		return OHC_OK;
	}
	if(*p != '\r') {
		goto fail;
	}

	r->range_set = 1;
	r->range_start = start;
	r->range_end = end;
	return OHC_OK;
fail:
	r->error_reason = "InvalidRange";
	return OHC_ERROR;
}

static int http_parse_connection(ohc_request_t *r, char *p, ssize_t len)
{
	if(strncmp(p, "close", 5) == 0) {
		r->keepalive = 0;
	}
	return OHC_OK;
}

#define GENERAL_HEADERS \
	{STRING_INIT("Host:"), http_parse_host}, \
	{STRING_INIT("OHC-Key:"),  http_parse_ohc_key}, \
	{STRING_INIT("Connection:"),  http_parse_connection}, \
	{STRING_INIT(""),  NULL}, 

static struct http_header_s http_request_header_get[] = {
	{STRING_INIT("Range:"), http_parse_get_range},
	GENERAL_HEADERS
};

static struct http_header_s http_request_header_put[] = {
	{STRING_INIT("Content-Length:"), http_parse_put_content_length},
	{STRING_INIT("Cache-Control:"), http_parse_put_cache_control},
	{STRING_INIT("Expires"), http_parse_put_expires},
	GENERAL_HEADERS
};

static struct http_header_s http_request_header_delete[] = {
	GENERAL_HEADERS
};

static void http_add_put_headers(ohc_request_t *r, char *base, ssize_t len)
{
	string_t *p;

	if(r->put_header_nr == 0) {
		r->put_headers[0].base = base;
		r->put_headers[0].len = len;
		r->put_header_nr = 1;
	} else {
		p = &r->put_headers[r->put_header_nr - 1];
		if(p->base + p->len == base) {
			p->len += len;
		} else {
			p++;
			p->base = base;
			p->len = len;
			r->put_header_nr++;
		}
	}
}

struct http_method_s http_methods[] = {
	{STRING_INIT("GET "), http_request_header_get},
	{STRING_INIT("HEAD "), http_request_header_get},
	{STRING_INIT("PUT "), http_request_header_put},
	{STRING_INIT("POST "), http_request_header_put},
	{STRING_INIT("PURGE "), http_request_header_delete},
	{STRING_INIT("DELETE "), http_request_header_delete},
	{STRING_INIT("XXX "), NULL}
};


/* parse HTTP request. do not mark the status if not complete, so 
 * work fast for one-packet-request, and slowly for others. */
int http_request_parse(ohc_request_t *r)
{
	struct http_header_s *headers = NULL, *h;
	struct http_method_s *method;
	char *p, *q;
	char *name, *value_base;
	ssize_t value_len;
	int rc = OHC_OK;
	int is_store;

	p = r->_buffer;
	*r->buf_pos = '\0';
	r->put_header_nr = 0;
	r->put_headers[0].base = NULL;
	r->put_headers[0].len = 0;
	r->put_header_length = 0;

	/* method */
	for(method = http_methods; method->data; method++) {
		if(strncmp(p, method->str.base, method->str.len) == 0) {
			r->method = method - http_methods;
			p += method->str.len;
			headers = method->data;
			break;
		}
	}
	if(method->data == NULL) {
		r->error_reason = "UnknownMethod";
		goto fail;
	}
	is_store = (headers == http_request_header_put);

	/* uri */
	while(*p == ' ') p++;
	if((q = strchr(p, ' ')) == NULL) {
		goto not_complete;
	}
	r->uri.base = p;
	r->uri.len = q - p;
	p = q + 1;

	/* HTTP/1.1 */
	while(*p == ' ') p++;
	if(strncasecmp(p, "HTTP/", 5) != 0) {
		r->error_reason = "NotHTTP";
		goto fail;
	}
	if((q = strchr(p + 5, '\r')) == NULL) {
		goto not_complete;
	}
	p = q + 2;

	/* headers */
	while(1) {
		while(*p == ' ') p++;
		if(*p == '\r' && *(p+1) == '\n') {
			break;
		}

		/* header name */
		if((q = strchr(p, ':')) == NULL) {
			goto not_complete;
		}
		name = p;
		p = q + 1;

		/* header value */
		while(*p == ' ') p++;
		if((q = strchr(p, '\r')) == NULL || *(q+1) == '\0') {
			goto not_complete;
		}
		value_base = p;
		value_len = q - p;
		p = q + 2;

		for(h = headers; h->handler; h++) {
			if(strncasecmp(name, h->name.base, h->name.len) == 0) {

				rc = h->handler(r, value_base, value_len);
				if(rc == OHC_ERROR) {
					goto fail;
				}
				break;
			}
		}

		/* if PUT/POST, record the un-handled headers */
		if(is_store && (h->handler == NULL || rc == OHC_DECLINE)) {

			http_add_put_headers(r, name, p - name);
			r->put_header_length += p - name;
		}
	}

	if(is_store) {
		if(r->content_length == -1) {
			r->error_reason = "NoContentLengthinPUT";
			goto fail;
		}

		if(r->buf_pos - p - 2 > r->content_length) {
			r->error_reason = "BodyLargerThanDeclared";
			goto fail;
		}

		/* add the pre-read body */
		http_add_put_headers(r, p, r->buf_pos - p);

		r->put_header_length += http_make_200_response_header(r->content_length, NULL);
		r->put_header_length += 2; /* "\r\n" */
	}

	return OHC_DONE;

not_complete:
	if(strstr(p, "\r\n\r\n")) {
		goto fail;
	}
	return OHC_AGAIN;

fail:
	r->keepalive = 0;
	r->http_code = 400;
	return OHC_ERROR;
}

string_t *http_code_page(int code)
{
#define HTTP_PAGE_CONLEN(code, desc) \
	{code, STRING_INIT("HTTP/1.1 " #code " " desc "\r\nContent-Length: 0\r\n\r\n")}
#define HTTP_PAGE_NOLEN(code, desc) \
	{code, STRING_INIT("HTTP/1.1 " #code " " desc "\r\n\r\n")}

	static struct http_page_s {
		int code;
		string_t page;
	} http_pages[] = {
		HTTP_PAGE_NOLEN (201, "Created"),
		HTTP_PAGE_NOLEN (204, "No Content"),
		HTTP_PAGE_CONLEN(400, "Bad Request"),
		HTTP_PAGE_CONLEN(404, "Not Found"),
		HTTP_PAGE_CONLEN(413, "Request Entity Too Large"),
		HTTP_PAGE_CONLEN(416, "Requested Range Not Satisfiable"),
		HTTP_PAGE_CONLEN(500, "Internal Server Error"),
	};
	int i;

	for(i = 0; i < sizeof(http_pages) / sizeof(struct http_page_s); i++) {
		if(http_pages[i].code == code) {
			return &http_pages[i].page;
		}
	}

	return NULL; /* should not happen */
}

ssize_t http_make_206_response_header(ssize_t range_start, ssize_t range_end,
		ssize_t body_len, char *output)
{
	return sprintf(output, "HTTP/1.1 206 Partial Content\r\n"
			"Content-Length: %ld\r\n"
			"Content-Range: bytes %ld-%ld/%ld\r\n",
			range_end - range_start + 1,
			range_start, range_end, body_len);
}

ssize_t http_make_200_response_header(ssize_t content_length, char *output)
{
#define RESP_200_CONLEN "HTTP/1.1 200 OK\r\nContent-Length: "
	if(output) {
		return sprintf(output, RESP_200_CONLEN "%ld\r\n", content_length);
	} else {
		return sizeof(RESP_200_CONLEN "\r\n") - 1 + numlen(content_length);
	}
}

ssize_t http_decode_uri(const char *uri, ssize_t len, char *output)
{
	int a, b;
	ssize_t i, output_len = 0;

	for(i = 0; i < len; i++) {
		if(uri[i] == '%') {
			a = char2hex(uri[i+1]);
			b = char2hex(uri[i+2]);
			if(a >= 0 && b >= 0) {
				output[output_len++] = (a << 4) + b;
				i += 2;
				continue;
			}
		}
		output[output_len++] = uri[i];
	}
	return output_len;
}
