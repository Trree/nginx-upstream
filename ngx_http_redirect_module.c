#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct
{
    ngx_http_status_t           status;
    ngx_str_t					redirectServer;
} ngx_http_redirect_ctx_t;

typedef struct
{
    ngx_http_upstream_conf_t upstream;
    ngx_str_t addr;
} ngx_http_redirect_conf_t;

static char *
ngx_http_redirect(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_http_redirect_handler(ngx_http_request_t *r);
static void* ngx_http_redirect_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_redirect_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static ngx_int_t
redirect_upstream_process_header(ngx_http_request_t *r);
static ngx_int_t
redirect_process_status_line(ngx_http_request_t *r);


static ngx_str_t  ngx_http_proxy_hide_headers[] =
{
    ngx_string("Date"),
    ngx_string("Server"),
    ngx_string("X-Pad"),
    ngx_string("X-Accel-Expires"),
    ngx_string("X-Accel-Redirect"),
    ngx_string("X-Accel-Limit-Rate"),
    ngx_string("X-Accel-Buffering"),
    ngx_string("X-Accel-Charset"),
    ngx_null_string
};


static ngx_command_t  ngx_http_redirect_commands[] =
{

    {
        ngx_string("redirect"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1,
        ngx_http_redirect,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_redirect_conf_t, addr),
        NULL
    },

    ngx_null_command
};

static ngx_http_module_t  ngx_http_redirect_module_ctx =
{
    NULL,                              /* preconfiguration */
    NULL,                        	   /* postconfiguration */

    NULL,                              /* create main configuration */
    NULL,                              /* init main configuration */

    NULL,                              /* create server configuration */
    NULL,                              /* merge server configuration */

    ngx_http_redirect_create_loc_conf,   /* create location configuration */
    ngx_http_redirect_merge_loc_conf     /* merge location configuration */
};

ngx_module_t  ngx_http_redirect_module =
{
    NGX_MODULE_V1,
    &ngx_http_redirect_module_ctx,           /* module context */
    ngx_http_redirect_commands,              /* module directives */
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


static void* ngx_http_redirect_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_redirect_conf_t  *mycf;

    mycf = (ngx_http_redirect_conf_t  *)ngx_pcalloc(cf->pool, sizeof(ngx_http_redirect_conf_t));
    if (mycf == NULL)
    {
        return NULL;
    }

    //以下简单的硬编码ngx_http_upstream_conf_t结构中的各成员，例如
    //超时时间都设为1分钟。这也是http反向代理模块的默认值
    mycf->upstream.connect_timeout = 60000;
    mycf->upstream.send_timeout = 60000;
    mycf->upstream.read_timeout = 60000;
    mycf->upstream.store_access = 0600;
    //实际上buffering已经决定了将以固定大小的内存作为缓冲区来转发上游的
    //响应包体，这块固定缓冲区的大小就是buffer_size。如果buffering为1
    //就会使用更多的内存缓存来不及发往下游的响应，例如最多使用bufs.num个
    //缓冲区、每个缓冲区大小为bufs.size，另外还会使用临时文件，临时文件的
    //最大长度为max_temp_file_size
    mycf->upstream.buffering = 0;
    mycf->upstream.bufs.num = 8;
    mycf->upstream.bufs.size = ngx_pagesize;
    mycf->upstream.buffer_size = ngx_pagesize;
    mycf->upstream.busy_buffers_size = 2 * ngx_pagesize;
    mycf->upstream.temp_file_write_size = 2 * ngx_pagesize;
    mycf->upstream.max_temp_file_size = 1024 * 1024 * 1024;

    //upstream模块要求hide_headers成员必须要初始化（upstream在解析
    //完上游服务器返回的包头时，会调用
    //ngx_http_upstream_process_headers方法按照hide_headers成员将
    //本应转发给下游的一些http头部隐藏），这里将它赋为
    //NGX_CONF_UNSET_PTR ，是为了在merge合并配置项方法中使用
    //upstream模块提供的ngx_http_upstream_hide_headers_hash
    //方法初始化hide_headers 成员
    mycf->upstream.hide_headers = NGX_CONF_UNSET_PTR;
    mycf->upstream.pass_headers = NGX_CONF_UNSET_PTR;
    
    //mycf->addr = {0, NULL};
    return mycf;
}


static char *ngx_http_redirect_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_redirect_conf_t *prev = (ngx_http_redirect_conf_t *)parent;
    ngx_http_redirect_conf_t *conf = (ngx_http_redirect_conf_t *)child;

    ngx_hash_init_t             hash;
    hash.max_size = 100;
    hash.bucket_size = 1024;
    hash.name = "proxy_headers_hash";
    if (ngx_http_upstream_hide_headers_hash(cf, &conf->upstream,
        &prev->upstream, ngx_http_proxy_hide_headers, &hash)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }
    ngx_conf_merge_str_value(conf->addr, conf->addr, ""); 

    return NGX_CONF_OK;
}


static ngx_int_t
redirect_upstream_create_request(ngx_http_request_t *r)
{
    static ngx_str_t redirectQueryLine = ngx_string("GET / HTTP/1.1\r\nHost: www.sina.com.cn\r\nConnection: close\r\n\r\n");
    //ngx_int_t queryLineLen = redirectQueryLine.len ;
    //必须由内存池中申请内存，这有两点好处：在网络情况不佳的情况下，向上游
    //服务器发送请求时，可能需要epoll多次调度send发送才能完成，
    //这时必须保证这段内存不会被释放；请求结束时，这段内存会被自动释放，
    //降低内存泄漏的可能
    ngx_buf_t* b = ngx_create_temp_buf(r->pool, redirectQueryLine.len);
    if (b == NULL)
        return NGX_ERROR;

    b->last = ngx_copy(b->last, redirectQueryLine.data, redirectQueryLine.len);
    //*b->last++ = CR; *b->last++ = LF;
    //last要指向请求的末尾
    //b->last = b->pos + queryLineLen;

    //作用相当于snprintf，只是它支持4.4节中的表4-7列出的所有转换格式
    //ngx_snprintf(b->pos, queryLineLen , (char*)redirectQueryLine.data, &r->args);
    // r->upstream->request_bufs是一个ngx_chain_t结构，它包含着要
    //发送给上游服务器的请求

    ngx_chain_t                  *cl;
    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    r->upstream->request_bufs = cl;
    cl->next = NULL;

    b->flush = 1;
    //r->upstream->request_sent = 0;
    //r->upstream->header_sent = 0;
    // header_hash不可以为0
    r->header_hash = 1;
    return NGX_OK;
}

static ngx_int_t
redirect_process_status_line(ngx_http_request_t *r)
{
    size_t                 len;
    ngx_int_t              rc;
    ngx_http_upstream_t   *u;

    //上下文中才会保存多次解析http响应行的状态，首先取出请求的上下文
    ngx_http_redirect_ctx_t* ctx = ngx_http_get_module_ctx(r, ngx_http_redirect_module);
    if (ctx == NULL)
    {
        return NGX_ERROR;
    }

    u = r->upstream;

    //http框架提供的ngx_http_parse_status_line方法可以解析http
    //响应行，它的输入就是收到的字符流和上下文中的ngx_http_status_t结构
    rc = ngx_http_parse_status_line(r, &u->buffer, &ctx->status);
    //返回NGX_AGAIN表示还没有解析出完整的http响应行，需要接收更多的
    //字符流再来解析
    if (rc == NGX_AGAIN)
    {
        return rc;
    }
    //返回NGX_ERROR则没有接收到合法的http响应行
    if (rc == NGX_ERROR)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream sent no valid HTTP/1.0 header");

        r->http_version = NGX_HTTP_VERSION_9;
        u->state->status = NGX_HTTP_OK;

        return NGX_OK;
    }

    //以下表示解析到完整的http响应行，这时会做一些简单的赋值操作，将解析出
    //的信息设置到r->upstream->headers_in结构体中，upstream解析完所
    //有的包头时，就会把headers_in中的成员设置到将要向下游发送的
    //r->headers_out结构体中，也就是说，现在我们向headers_in中设置的
    //信息，最终都会发往下游客户端。为什么不是直接设置r->headers_out而要
    //这样多此一举呢？这是因为upstream希望能够按照
    //ngx_http_upstream_conf_t配置结构体中的hide_headers等成员对
    //发往下游的响应头部做统一处理
    if (u->state)
    {
        u->state->status = ctx->status.code;
    }

    u->headers_in.status_n = ctx->status.code;

    len = ctx->status.end - ctx->status.start;
    u->headers_in.status_line.len = len;

    u->headers_in.status_line.data = ngx_pnalloc(r->pool, len);
    if (u->headers_in.status_line.data == NULL)
    {
        return NGX_ERROR;
    }

    ngx_memcpy(u->headers_in.status_line.data, ctx->status.start, len);

    //下一步将开始解析http头部，设置process_header回调方法为
    //redirect_upstream_process_header，
    //之后再收到的新字符流将由redirect_upstream_process_header解析
    u->process_header = redirect_upstream_process_header;

    //如果本次收到的字符流除了http响应行外，还有多余的字符，
    //将由redirect_upstream_process_header方法解析
    return redirect_upstream_process_header(r);
}


static ngx_int_t
redirect_upstream_process_header(ngx_http_request_t *r)
{
    ngx_int_t                       rc;
    ngx_table_elt_t                *h;
    ngx_http_upstream_header_t     *hh;
    ngx_http_upstream_main_conf_t  *umcf;

    //这里将upstream模块配置项ngx_http_upstream_main_conf_t取了
    //出来，目的只有1个，对将要转发给下游客户端的http响应头部作统一
    //处理。该结构体中存储了需要做统一处理的http头部名称和回调方法
    umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);

    //循环的解析所有的http头部
    for ( ;; )
    {
        // http框架提供了基础性的ngx_http_parse_header_line
        //方法，它用于解析http头部
        rc = ngx_http_parse_header_line(r, &r->upstream->buffer, 1);
        //返回NGX_OK表示解析出一行http头部
        if (rc == NGX_OK)
        {
            //向headers_in.headers这个ngx_list_t链表中添加http头部
            h = ngx_list_push(&r->upstream->headers_in.headers);
            if (h == NULL)
            {
                return NGX_ERROR;
            }
            //以下开始构造刚刚添加到headers链表中的http头部
            h->hash = r->header_hash;

            h->key.len = r->header_name_end - r->header_name_start;
            h->value.len = r->header_end - r->header_start;
            //必须由内存池中分配存放http头部的内存
            h->key.data = ngx_pnalloc(r->pool,
                                      h->key.len + 1 + h->value.len + 1 + h->key.len);
            if (h->key.data == NULL)
            {
                return NGX_ERROR;
            }

            h->value.data = h->key.data + h->key.len + 1;
            h->lowcase_key = h->key.data + h->key.len + 1 + h->value.len + 1;

            ngx_memcpy(h->key.data, r->header_name_start, h->key.len);
            h->key.data[h->key.len] = '\0';
            ngx_memcpy(h->value.data, r->header_start, h->value.len);
            h->value.data[h->value.len] = '\0';

            if (h->key.len == r->lowcase_index)
            {
                ngx_memcpy(h->lowcase_key, r->lowcase_header, h->key.len);
            }
            else
            {
                ngx_strlow(h->lowcase_key, h->key.data, h->key.len);
            }

            //upstream模块会对一些http头部做特殊处理
            hh = ngx_hash_find(&umcf->headers_in_hash, h->hash,
                               h->lowcase_key, h->key.len);

            if (hh && hh->handler(r, h, hh->offset) != NGX_OK)
            {
                return NGX_ERROR;
            }

            continue;
        }

        //返回NGX_HTTP_PARSE_HEADER_DONE表示响应中所有的http头部都解析
        //完毕，接下来再接收到的都将是http包体
        if (rc == NGX_HTTP_PARSE_HEADER_DONE)
        {
            //如果之前解析http头部时没有发现server和date头部，以下会
            //根据http协议添加这两个头部
            if (r->upstream->headers_in.server == NULL)
            {
                h = ngx_list_push(&r->upstream->headers_in.headers);
                if (h == NULL)
                {
                    return NGX_ERROR;
                }

                h->hash = ngx_hash(ngx_hash(ngx_hash(ngx_hash(
                                   ngx_hash('s', 'e'), 'r'), 'v'), 'e'), 'r');

                ngx_str_set(&h->key, "Server");
                ngx_str_null(&h->value);
                h->lowcase_key = (u_char *) "server";
            }

            if (r->upstream->headers_in.date == NULL)
            {
                h = ngx_list_push(&r->upstream->headers_in.headers);
                if (h == NULL)
                {
                    return NGX_ERROR;
                }

                h->hash = ngx_hash(ngx_hash(ngx_hash('d', 'a'), 't'), 'e');

                ngx_str_set(&h->key, "Date");
                ngx_str_null(&h->value);
                h->lowcase_key = (u_char *) "date";
            }

            return NGX_OK;
        }

        //如果返回NGX_AGAIN则表示状态机还没有解析到完整的http头部，
        //要求upstream模块继续接收新的字符流再交由process_header
        //回调方法解析
        if (rc == NGX_AGAIN)
        {
            return NGX_AGAIN;
        }

        //其他返回值都是非法的
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream sent invalid header");

        return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }
}

static void
redirect_upstream_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "redirect_upstream_finalize_request");
}

static char *
ngx_http_redirect(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_redirect_handler;

    ngx_http_redirect_conf_t *arcf = conf;
    ngx_str_t *value;

    if (arcf->addr.data != NULL) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "off") == 0) {
        arcf->addr.len = 0;
        arcf->addr.data = (u_char *) "";

        return NGX_CONF_OK;
    }

    arcf->addr = value[1];

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_redirect_handler(ngx_http_request_t *r)
{
    ngx_http_redirect_ctx_t* myctx = ngx_http_get_module_ctx(r, ngx_http_redirect_module);
    if (myctx == NULL)
    {
        myctx = ngx_palloc(r->pool, sizeof(ngx_http_redirect_ctx_t));
        if (myctx == NULL)
        {
            return NGX_ERROR;
        }
        //将新建的上下文与请求关联起来
        ngx_http_set_ctx(r, myctx, ngx_http_redirect_module);
    }
    //对每1个要使用upstream的请求，必须调用且只能调用1次
    //ngx_http_upstream_create方法，它会初始化r->upstream成员
    if (ngx_http_upstream_create(r) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_upstream_create() failed");
        return NGX_ERROR;
    }

    //得到配置结构体ngx_http_redirect_conf_t
    ngx_http_redirect_conf_t  *mycf = (ngx_http_redirect_conf_t  *) ngx_http_get_module_loc_conf(r, ngx_http_redirect_module);
    ngx_http_upstream_t *u = r->upstream;
    //这里用配置文件中的结构体来赋给r->upstream->conf成员
    u->conf = &mycf->upstream;
    //决定转发包体时使用的缓冲区
    u->buffering = mycf->upstream.buffering;


    ngx_url_t url;
    ngx_memzero(&url, sizeof(ngx_url_t));
    url.url = mycf->addr;
    url.default_port = 80;

    if (ngx_parse_url(r->pool, &url) != NGX_OK) {
        if (url.err) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "%s in upstream \"%V\"", url.err, &url.url);
        }

        return NGX_ERROR;
    }
    
    u_char *p;
    if (url.uri.len) {
        if (url.uri.data[0] == '?') {
            p = ngx_pnalloc(r->pool, url.uri.len + 1);
            if (p == NULL) {
                return NGX_ERROR;
            }

            *p++ = '/';
            ngx_memcpy(p, url.uri.data, url.uri.len);

            url.uri.len++;
            url.uri.data = p - 1;
        }
    }

    u->resolved = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_resolved_t));
    if (u->resolved == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_pcalloc resolved error. %s.", strerror(errno));
        return NGX_ERROR;
    }

    if (url.addrs) {
        u->resolved->sockaddr = url.addrs[0].sockaddr;
        u->resolved->socklen = url.addrs[0].socklen;
        u->resolved->name = url.addrs[0].name;
        u->resolved->naddrs = 1;
    }

    u->resolved->host = url.host;
    u_short port = 80;
    u->resolved->port = (in_port_t) (url.no_port ? port : url.port);
    u->resolved->no_port = url.no_port;

    myctx->redirectServer= url.host;


    //设置三个必须实现的回调方法，也就是5.3.3节至5.3.5节中实现的3个方法
    u->create_request = redirect_upstream_create_request;
    u->process_header = redirect_process_status_line;
    u->finalize_request = redirect_upstream_finalize_request;

    //这里必须将count成员加1，理由见5.1.5节
    r->main->count++;
    //启动upstream
    ngx_http_upstream_init(r);
    return NGX_DONE;
}


