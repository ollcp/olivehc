/*
 *
 * store response to a upstream
 *
 *  Created on: 2011-11-26
 *      Author: Wu Bingzheng
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
    /* response body chain */
    ngx_chain_t  *buf_chain_head;
    ngx_chain_t  *buf_chain_tail;

    off_t        copied_length;

    unsigned     subr_sent:1;

    unsigned     store_enable:1;
} ngx_http_jstore_ctx_t;

typedef struct {
    ngx_str_t    target;
    off_t        max_size;
    ngx_flag_t   check_cacheable;
} ngx_http_jstore_loc_conf_t;

static ngx_command_t ngx_http_jstore_commands[] = {
    {
        ngx_string("jstore"),
        NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_jstore_loc_conf_t, target),
        NULL
    },
    {
        ngx_string("jstore_max_size"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_size_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_jstore_loc_conf_t, max_size),
        NULL
    },
    {
        ngx_string("jstore_check_cacheable"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_jstore_loc_conf_t, check_cacheable),
        NULL
    },
    ngx_null_command
};

static ngx_int_t ngx_http_jstore_init(ngx_conf_t *cf);
static void *ngx_http_jstore_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_jstore_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);


static ngx_http_module_t  ngx_http_jstore_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_jstore_init,                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_jstore_create_loc_conf,       /* create location configuration */
    ngx_http_jstore_merge_loc_conf,        /* merge location configuration */
};

ngx_module_t  ngx_http_jstore_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_jstore_filter_module_ctx,    /* module context */
    ngx_http_jstore_commands,              /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_body_filter_pt  ngx_http_next_body_filter;
static ngx_http_output_header_filter_pt ngx_http_next_header_filter;

/* the space following "POST" is nessicery because nginx copy it; 
 * the comment following space is used as marking one request is
 * jstore's subrequest. */
#define JSUBR_METHOD_STR "POST #jstore-subr"
#define JSUBR_METHOD_LEN 4

static ngx_int_t
ngx_http_jstore_is_subrequest(ngx_http_request_t *r)
{
    return (r != r->main) && (r->method_name.data == (u_char *)JSUBR_METHOD_STR);
}

static ngx_int_t
ngx_http_jstore_handler(ngx_http_request_t *r)
{
    ngx_http_jstore_loc_conf_t *jlcf;
    ngx_http_jstore_ctx_t *ctx;

    jlcf = ngx_http_get_module_loc_conf(r, ngx_http_jstore_filter_module);
    if(jlcf == NULL || jlcf->target.data == NULL) {
        return NGX_DECLINED;
    }

    /* store only when the last status is 404, which means cache miss;
     * while not store if 50x, which means cache fails. */
    if(r->upstream == NULL || r->upstream->headers_in.status_n != 404) {
        return NGX_DECLINED;
    }

    /* check only once. */
    ctx = ngx_http_get_module_ctx(r, ngx_http_jstore_filter_module);
    if(ctx != NULL) {
        ctx->store_enable = 0;
        return NGX_DECLINED;
    }

    ctx = ngx_palloc(r->pool, sizeof(ngx_http_jstore_ctx_t));
    if(ctx == NULL) {
        return NGX_DECLINED;
    }

    /* other members will be init later */
    ctx->store_enable = 0;
    ngx_http_set_ctx(r, ctx, ngx_http_jstore_filter_module);

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_jstore_check_cacheable(ngx_http_request_t *r)
{
    ngx_array_t *cache_control;
    ngx_table_elt_t *h;
    ngx_uint_t i;
    time_t expires;

#if (NGX_HTTP_CACHE)
    if(r->cache && r->cache->valid_sec != 0) {
        return 1;
    }
#endif
    if(r->upstream->cacheable) {
        return 1;
    }

    cache_control = &r->upstream->headers_in.cache_control;
    if(cache_control && cache_control->elts) {
        h = *((ngx_table_elt_t **)cache_control->elts);
        for(i = 0; i < cache_control->nelts; i++, h++) {

            if(ngx_strncasecmp(h->value.data, "max-age=", 8) != 0
                    || h->value.data[8] == '0') {
                return 0;
            }
        }
    }

    h = r->upstream->headers_in.expires;
    if(h) {
        expires = ngx_http_parse_time(h->value.data, h->value.len);
        if (expires == NGX_ERROR || expires < ngx_time()) {
            return 0;
        }
    }

    return 1;
}

static ngx_int_t
ngx_http_jstore_header_filter(ngx_http_request_t *r)
{
    ngx_http_jstore_loc_conf_t *jlcf;
    ngx_http_jstore_ctx_t *ctx;
    off_t content_length;

    if(ngx_http_jstore_is_subrequest(r)) {
        goto pipe;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_jstore_filter_module);
    if(ctx == NULL) {
        goto pipe;
    }

    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0, "jstore header filter");

    jlcf = ngx_http_get_module_loc_conf(r, ngx_http_jstore_filter_module);

    content_length = r->headers_out.content_length_n;
    if(content_length > jlcf->max_size
            || (content_length < 0 && !r->upstream->headers_in.chunked)
            || r->headers_out.status != NGX_HTTP_OK
            || (r->headers_in.range != NULL && r->allow_ranges)
            || r->header_only) {

        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "jstore: not store1");
        goto pipe;
    }

    if(jlcf->check_cacheable && !ngx_http_jstore_check_cacheable(r)) {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "jstore: not store2");
        goto pipe;
    }

    ctx->store_enable = 1;
    ctx->buf_chain_head = NULL;
    ctx->buf_chain_tail = NULL;
    ctx->copied_length = 0;
    ctx->subr_sent = 0;

pipe:
    return ngx_http_next_header_filter(r);
}

static ngx_int_t
ngx_http_jstore_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_core_main_conf_t *cmcf;
    ngx_http_jstore_loc_conf_t *jlcf;
    ngx_http_jstore_ctx_t *ctx;
    ngx_http_request_t *sr = NULL;
    ngx_http_upstream_t *u;
    ngx_chain_t *cl, *jchain;
    ngx_buf_t *buf, *jbuf;
    ngx_int_t rc, rc_next;
    ssize_t n;
    off_t clen;
    off_t content_length = r->headers_out.content_length_n;

    if(in == NULL) {
        goto pipe;
    }

    /* skip jstore's subrequest's response body */
    if(ngx_http_jstore_is_subrequest(r)) {
        for(cl = in; cl; cl = cl->next) {
            buf = cl->buf;

            buf->file_pos = buf->file_last;
            buf->pos = buf->last;  
            buf->sync = 1;             
            buf->memory = 0;           
            buf->in_file = 0;          
        }
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0, "jstore body filter");

    ctx = ngx_http_get_module_ctx(r, ngx_http_jstore_filter_module);
    if(ctx == NULL || !ctx->store_enable || ctx->subr_sent) {
        goto pipe;
    }

    for(cl = in; cl; cl = cl->next) {

        buf = cl->buf;
        if(ngx_buf_special(buf)) {
            continue;
        }

        clen = ngx_buf_size(buf);

        if(content_length >= 0 && clen + ctx->copied_length > content_length) {
            ngx_log_error(NGX_LOG_ERR,r->connection->log, 0,
                    "body(%d) is larger than Content-Length(%d)",
                    clen + ctx->copied_length, content_length);
            goto fail;
        }

        /* alloc new buffer, and link it */
        jbuf = ngx_create_temp_buf(r->pool, clen);
        if(jbuf == NULL) {
            goto fail;
        }

        jchain = ngx_alloc_chain_link(r->pool);
        if(jchain == NULL) {
            goto pipe;
        }

        if(ctx->buf_chain_head == NULL) {
            ctx->buf_chain_head = jchain;
        } else {
            ctx->buf_chain_tail->next = jchain;
        }
        ctx->buf_chain_tail = jchain;
        jchain->buf = jbuf;
        jchain->next = NULL;

        /* copy content */
        if(buf->in_file && buf->file){
            n = ngx_read_file(buf->file, jbuf->start, clen, buf->file_pos);
            if(n < 0){
                ngx_log_error(NGX_LOG_ERR, r->connection->log,
                        0, "ngx_read_file failed! ret: %d", n);
                goto fail;
            }
            buf->file->offset -= n;

        } else {
            ngx_memcpy(jbuf->start, buf->pos, clen);
        }

        jbuf->last += clen;
        ctx->copied_length += clen;
    }

    /* upstream done? buffering and non-buffering */
    u = r->upstream;
    if(!(u->pipe && u->pipe->upstream_done) && u->length != 0) {
        goto pipe;
    }

    /* should not happen */
    if(content_length >= 0 && ctx->copied_length != content_length) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0, "JSTORE bug!!");
        goto fail;
    }

    /* now we have collect all response, so send a subrequest to store it */

    /* send the main request first */
    rc_next = ngx_http_next_body_filter(r, in);

    ctx->subr_sent = 1;

    /* create subrequest */
    rc = ngx_http_subrequest(r, &(r->uri), &(r->args), &sr, NULL, 0);
    if(rc == NGX_ERROR) {
        goto fail;
    }

    /* method */
    sr->method = NGX_HTTP_POST;
    sr->method_name.data = JSUBR_METHOD_STR;
    sr->method_name.len = JSUBR_METHOD_LEN;

    /* request headers */
    sr->headers_in.headers = u->headers_in.headers;
    sr->headers_in.content_length_n = ctx->copied_length;

    /* $proxy_internal_body_length is cacheable, and the subrequest
     * has different content-lenght with main request, so we have
     * to clear the variables. */
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);
    sr->variables = ngx_pcalloc(r->pool, cmcf->variables.nelts
                                        * sizeof(ngx_http_variable_value_t));

    /* request body */
    sr->request_body = ngx_pcalloc(r->pool, sizeof(ngx_http_request_body_t));
    if(sr->request_body == NULL) {
        goto fail;
    }

    sr->request_body->bufs = ctx->buf_chain_head;

    /* ngx_http_subrequest() and ngx_http_named_location() both
     * increase @r->count, but we want it to be incresed once 
     * actually. */
    r->count--;

    /* jump to the named location */
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0, "jstore: named-location");
    jlcf = ngx_http_get_module_loc_conf(r, ngx_http_jstore_filter_module);
    ngx_http_named_location(sr, &jlcf->target);

    return rc_next;

fail:
    ngx_http_set_ctx(r, NULL, ngx_http_jstore_filter_module);
pipe:
    return ngx_http_next_body_filter(r, in);
}


static char *
ngx_http_jstore_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_jstore_loc_conf_t *prev = parent;
    ngx_http_jstore_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->max_size, prev->max_size, 100<<20); /* 100M */
    ngx_conf_merge_value(conf->check_cacheable, prev->check_cacheable, 1);
    return NGX_CONF_OK;
}

static void *
ngx_http_jstore_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_jstore_loc_conf_t *jlcf;
    jlcf = ngx_palloc(cf->pool, sizeof(ngx_http_jstore_loc_conf_t));
    if(jlcf == NULL) {
        return NGX_CONF_ERROR;
    }

    jlcf->target.data = NULL;
    jlcf->max_size = NGX_CONF_UNSET_SIZE;
    jlcf->check_cacheable = NGX_CONF_UNSET;
    return jlcf;
}


static ngx_int_t
ngx_http_jstore_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    /* handler part */
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_http_jstore_handler;

    /* filter part */
    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_jstore_body_filter;

    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_jstore_header_filter;

    return NGX_OK;
}

/* vim: set tw=0 shiftwidth=4 tabstop=4 expandtab: */
