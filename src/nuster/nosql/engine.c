/*
 * nuster nosql engine functions.
 *
 * Copyright (C) Jiang Wenyuan, < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <inttypes.h>

#include <nuster/memory.h>
#include <nuster/shctx.h>
#include <nuster/nuster.h>
#include <nuster/http.h>

#include <types/global.h>
#include <types/stream.h>
#include <types/channel.h>
#include <types/proxy.h>

#include <proto/stream_interface.h>
#include <proto/http_ana.h>
#include <proto/acl.h>
#include <proto/log.h>
#include <proto/proxy.h>
#include <proto/http_htx.h>
#include <common/htx.h>

static void nst_res_simple2(struct stream *s, int status) {

    struct channel *res = &s->res;
    struct htx *htx = htx_from_buf(&res->buf);
    struct htx_sl *sl;
    struct ist code;
    unsigned int flags = (HTX_SL_F_IS_RESP|HTX_SL_F_VER_11);

    switch(status) {
        case 200:
            code = ist("200");
            break;
        case 500:
            code = ist("500");
            break;
        case 405:
            code = ist("405");
            break;
        case 404:
            code = ist("404");
            break;
        case 400:
            code = ist("400");
            break;
        case 507:
            code = ist("507");
            break;
        default:
            code = ist("500");
    }

    sl = htx_add_stline(htx, HTX_BLK_RES_SL, flags, ist("HTTP/1.1"), code,
            ist2(http_get_reason(status), strlen(http_get_reason(status))));

    if(!sl) {
        goto fail;
    }

    sl->info.res.status = status;
    s->txn->status = status;

    if(!htx_add_endof(htx, HTX_BLK_EOH)) {
        goto fail;
    }

    if(!htx_add_endof(htx, HTX_BLK_EOM)) {
        goto fail;
    }

    channel_add_input(res, htx->data);
    htx_to_buf(htx, &res->buf);
    return;

fail:
    channel_htx_truncate(res, htx);
}

static int _nst_nosql_element_to_htx(struct nst_nosql_element *element,
        struct htx *htx) {

    struct htx_blk *blk;
    char *ptr;
    uint32_t blksz, sz, info;
    enum htx_blk_type type;

    info = element->msg.len;
    type = (info >> 28);
    blksz = ((type == HTX_BLK_HDR || type == HTX_BLK_TLR)
            ? (info & 0xff) + ((info >> 8) & 0xfffff)
            : info & 0xfffffff);

    blk = htx_add_blk(htx, type, blksz);

    if(!blk) {
        return NST_ERR;
    }

    blk->info = info;
    ptr = htx_get_blk_ptr(htx, blk);
    sz = htx_get_blksz(blk);
    memcpy(ptr, element->msg.data, sz);

    return NST_OK;
}

static void nst_nosql_engine_handler(struct appctx *appctx) {
    struct stream_interface *si       = appctx->owner;
    struct stream *s                  = si_strm(si);
    struct channel *req               = si_oc(si);
    struct channel *res               = si_ic(si);
    struct nst_nosql_element *element = NULL;
    struct htx *req_htx, *res_htx;
    int ret;
    int total = 0;
    res_htx = htx_from_buf(&res->buf);

    if(unlikely(si->state == SI_ST_DIS || si->state == SI_ST_CLO)) {
        appctx->ctx.nuster.nosql_engine.data->clients--;
        return;
    }

    /* Check if the input buffer is avalaible. */
    if (!b_size(&res->buf)) {
        si_rx_room_blk(si);
        return;
    }

    /* check that the output is not closed */
    if(res->flags & (CF_SHUTW|CF_SHUTW_NOW)) {
        appctx->st0 = NST_NOSQL_CTX_STATE_DONE;
    }

    switch(appctx->st0) {
        case NST_NOSQL_APPCTX_STATE_CREATE:
            if (co_data(req)) {
                req_htx = htx_from_buf(&req->buf);
                co_htx_skip(req, req_htx, co_data(req));
                htx_to_buf(req_htx, &req->buf);
            }
            task_wakeup(s->task, TASK_WOKEN_OTHER);
            break;
        case NST_NOSQL_APPCTX_STATE_HIT:
            if(appctx->ctx.nuster.nosql_engine.element) {
                element = appctx->ctx.nuster.nosql_engine.element;

                while(element) {
                    if(_nst_nosql_element_to_htx(element, res_htx) != NST_OK) {
                        si_rx_room_blk(si);
                        goto out;
                    }

                    element = element->next;

                }

            } else {

                if (!htx_add_endof(res_htx, HTX_BLK_EOM)) {
                    si_rx_room_blk(si);
                    goto out;
                }

                if (!(res->flags & CF_SHUTR) ) {
                    res->flags |= CF_READ_NULL;
                    si_shutr(si);
                }

                /* eat the whole request */
                if (co_data(req)) {
                    req_htx = htx_from_buf(&req->buf);
                    co_htx_skip(req, req_htx, co_data(req));
                    htx_to_buf(req_htx, &req->buf);
                }
            }

out:
            appctx->ctx.nuster.nosql_engine.element = element;
            total = res_htx->data - total;
            channel_add_input(res, total);
            htx_to_buf(res_htx, &res->buf);
            break;
        case NST_NOSQL_APPCTX_STATE_HIT_DISK:
            {
                int max = b_room(&res->buf) - global.tune.maxrewrite;

                int fd = appctx->ctx.nuster.nosql_engine.fd;
                int header_len = appctx->ctx.nuster.nosql_engine.header_len;
                uint64_t offset = appctx->ctx.nuster.nosql_engine.offset;

                switch(appctx->st1) {
                    case NST_PERSIST_APPLET_HEADER:
                        {
                        char *p = trash.area;
                        ret = pread(fd, p, header_len, offset);

                        if(ret != header_len) {
                            appctx->st1 = NST_PERSIST_APPLET_ERROR;
                            break;
                        }

                        while(header_len != 0) {
                            struct htx_blk *blk;
                            char *ptr;
                            uint32_t blksz, sz, info;
                            enum htx_blk_type type;

                            info = *(uint32_t *)p;
                            type = (info >> 28);
                            blksz = (info & 0xff) + ((info >> 8) & 0xfffff);

                            blk = htx_add_blk(res_htx, type, blksz);

                            if(!blk) {
                                appctx->st1 = NST_PERSIST_APPLET_ERROR;
                                break;
                            }

                            blk->info = info;
                            ptr = htx_get_blk_ptr(res_htx, blk);
                            sz = htx_get_blksz(blk);
                            p += 4;
                            memcpy(ptr, p, sz);
                            p += sz;

                            header_len -= 4 + sz;
                        }

                        appctx->st1 = NST_PERSIST_APPLET_PAYLOAD;
                        offset += ret;
                        appctx->ctx.nuster.nosql_engine.offset += ret;
                        }

                        break;
                    case NST_PERSIST_APPLET_PAYLOAD:
                        max = htx_get_max_blksz(res_htx,
                                channel_htx_recv_max(res, res_htx));

                        ret = pread(fd, trash.area, max, offset);

                        if(ret == -1) {
                            appctx->st1 = NST_PERSIST_APPLET_ERROR;
                            break;
                        }

                        if(ret > 0) {
                            struct htx_blk *blk;
                            char *ptr;
                            uint32_t blksz, sz, info;
                            enum htx_blk_type type;

                            type = HTX_BLK_DATA;
                            info = (type << 28) + ret;
                            blksz = info & 0xfffffff;

                            blk = htx_add_blk(res_htx, type, blksz);

                            if(!blk) {
                                appctx->st1 = NST_PERSIST_APPLET_ERROR;
                                break;
                            }

                            blk->info = info;
                            ptr = htx_get_blk_ptr(res_htx, blk);
                            sz = htx_get_blksz(blk);
                            memcpy(ptr, trash.area, sz);

                            appctx->ctx.nuster.nosql_engine.offset += ret;
                            break;
                        }

                        close(fd);

                        appctx->st1 = NST_PERSIST_APPLET_EOM;
                    case NST_PERSIST_APPLET_EOM:

                        if (!htx_add_endof(res_htx, HTX_BLK_EOM)) {
                            si_rx_room_blk(si);
                            goto out2;
                        }

                        appctx->st1 = NST_PERSIST_APPLET_DONE;
                    case NST_PERSIST_APPLET_DONE:

                        if (!(res->flags & CF_SHUTR) ) {
                            res->flags |= CF_READ_NULL;
                            si_shutr(si);
                        }

                        if (co_data(req)) {
                            req_htx = htx_from_buf(&req->buf);
                            co_htx_skip(req, req_htx, co_data(req));
                            htx_to_buf(req_htx, &req->buf);
                        }

                        break;
                    case NST_PERSIST_APPLET_ERROR:
                        si_shutr(si);
                        res->flags |= CF_READ_NULL;
                        close(fd);
                        break;
                }
            }

out2:
            total = res_htx->data - total;
            channel_add_input(res, total);
            htx_to_buf(res_htx, &res->buf);
            task_wakeup(s->task, TASK_WOKEN_OTHER);
            break;
        case NST_NOSQL_APPCTX_STATE_ERROR:
            appctx->st0 = NST_NOSQL_APPCTX_STATE_DONE;
            nst_res_simple2(s, 500);
            break;
        case NST_NOSQL_APPCTX_STATE_NOT_ALLOWED:
            appctx->st0 = NST_NOSQL_APPCTX_STATE_DONE;
            nst_res_simple2(s, 405);
            break;
        case NST_NOSQL_APPCTX_STATE_NOT_FOUND:
            appctx->st0 = NST_NOSQL_APPCTX_STATE_DONE;
            nst_res_simple2(s, 404);
            break;
        case NST_NOSQL_APPCTX_STATE_EMPTY:
            appctx->st0 = NST_NOSQL_APPCTX_STATE_DONE;
            nst_res_simple2(s, 400);
            break;
        case NST_NOSQL_APPCTX_STATE_FULL:
            appctx->st0 = NST_NOSQL_APPCTX_STATE_DONE;
            nst_res_simple2(s, 507);
            break;
        case NST_NOSQL_APPCTX_STATE_END:
            appctx->st0 = NST_NOSQL_APPCTX_STATE_DONE;
            nst_res_simple2(s, 200);
            break;
        case NST_NOSQL_APPCTX_STATE_WAIT:
            break;
        case NST_NOSQL_APPCTX_STATE_DONE:
            if (!(res->flags & CF_SHUTR) ) {
                res->flags |= CF_READ_NULL;
                si_shutr(si);
            }

            /* eat the whole request */
            if (co_data(req)) {
                req_htx = htx_from_buf(&req->buf);
                co_htx_skip(req, req_htx, co_data(req));
                htx_to_buf(req_htx, &req->buf);
            }
            break;
        default:
            co_skip(si_oc(si), co_data(si_oc(si)));
            break;
    }

    return;
}

struct nst_nosql_data *nst_nosql_data_new() {
    struct nst_nosql_data *data = nst_nosql_memory_alloc(sizeof(*data));

    nst_shctx_lock(nuster.nosql);

    if(data) {
        data->clients = 0;
        data->invalid = 0;
        data->element = NULL;

        data->info.transfer_encoding.data = NULL;
        data->info.transfer_encoding.len  = 0;
        data->info.content_type.data      = NULL;
        data->info.content_type.len       = 0;
        data->info.content_length         = 0;

        if(nuster.nosql->data_head == NULL) {
            nuster.nosql->data_head = data;
            nuster.nosql->data_tail = data;
            data->next              = data;
        } else {

            if(nuster.nosql->data_head == nuster.nosql->data_tail) {
                nuster.nosql->data_head->next = data;
                data->next                    = nuster.nosql->data_head;
                nuster.nosql->data_tail       = data;
            } else {
                data->next                    = nuster.nosql->data_head;
                nuster.nosql->data_tail->next = data;
                nuster.nosql->data_tail       = data;
            }
        }
    }

    nst_shctx_unlock(nuster.nosql);

    return data;
}

static int _nst_nosql_data_invalid(struct nst_nosql_data *data) {

    if(data->invalid) {

        if(!data->clients) {
            return 1;
        }
    }
    return 0;
}

static void _nst_nosql_data_cleanup() {
    struct nst_nosql_data *data = NULL;

    if(nuster.nosql->data_head) {

        if(nuster.nosql->data_head == nuster.nosql->data_tail) {

            if(_nst_nosql_data_invalid(nuster.nosql->data_head)) {
                data                    = nuster.nosql->data_head;
                nuster.nosql->data_head = NULL;
                nuster.nosql->data_tail = NULL;
            }

        } else {

            if(_nst_nosql_data_invalid(nuster.nosql->data_head)) {
                data                          = nuster.nosql->data_head;
                nuster.nosql->data_tail->next = nuster.nosql->data_head->next;
                nuster.nosql->data_head       = nuster.nosql->data_head->next;
            } else {
                nuster.nosql->data_tail = nuster.nosql->data_head;
                nuster.nosql->data_head = nuster.nosql->data_head->next;
            }

        }
    }

    if(data) {
        struct nst_nosql_element *element = data->element;

        while(element) {
            struct nst_nosql_element *tmp = element;
            element                       = element->next;

            if(tmp->msg.data) {
                nst_nosql_stats_update_used_mem(-tmp->msg.len);
                nst_nosql_memory_free(tmp->msg.data);
            }

            nst_nosql_memory_free(tmp);
        }

        if(data->info.content_type.data) {
            nst_nosql_memory_free(data->info.content_type.data);
        }

        if(data->info.transfer_encoding.data) {
            nst_nosql_memory_free(data->info.transfer_encoding.data);
        }

        nst_nosql_memory_free(data);
    }
}

void nst_nosql_housekeeping() {

    if(global.nuster.nosql.status == NST_STATUS_ON && master == 1) {

        int dict_cleaner = global.nuster.nosql.dict_cleaner;
        int data_cleaner = global.nuster.nosql.data_cleaner;
        int disk_cleaner = global.nuster.nosql.disk_cleaner;
        int disk_loader  = global.nuster.nosql.disk_loader;
        int disk_saver   = global.nuster.nosql.disk_saver;

        while(dict_cleaner--) {
            nst_shctx_lock(&nuster.nosql->dict[0]);
            nst_nosql_dict_cleanup();
            nst_shctx_unlock(&nuster.nosql->dict[0]);
        }

        while(data_cleaner--) {
            nst_shctx_lock(nuster.nosql);
            _nst_nosql_data_cleanup();
            nst_shctx_unlock(nuster.nosql);
        }

        while(disk_cleaner--) {
            nst_nosql_persist_cleanup();
        }

        while(disk_loader--) {
            nst_nosql_persist_load();
        }

        disk_saver=1000;
        while(disk_saver--) {
            nst_shctx_lock(&nuster.nosql->dict[0]);
            nst_nosql_persist_async();
            nst_shctx_unlock(&nuster.nosql->dict[0]);
        }
    }
}

void nst_nosql_init() {
    nuster.applet.nosql_engine.fct = nst_nosql_engine_handler;

    if(global.nuster.nosql.status == NST_STATUS_ON) {

        if(global.nuster.nosql.root) {

            if(nst_persist_mkdir(global.nuster.nosql.root) == NST_ERR) {

                ha_alert("Create `%s` failed\n", global.nuster.nosql.root);
                exit(1);
            }
        }

        global.nuster.nosql.pool.ctx   = create_pool("np.ctx",
                sizeof(struct nst_nosql_ctx), MEM_F_SHARED);

        global.nuster.nosql.memory = nst_memory_create("nosql.shm",
                global.nuster.nosql.dict_size + global.nuster.nosql.data_size,
                global.tune.bufsize, NST_NOSQL_DEFAULT_CHUNK_SIZE);

        if(!global.nuster.nosql.memory) {
            goto shm_err;
        }

        if(nst_shctx_init(global.nuster.nosql.memory) != NST_OK) {
            goto shm_err;
        }

        nuster.nosql = nst_nosql_memory_alloc(sizeof(struct nst_nosql));

        if(!nuster.nosql) {
            goto err;
        }

        memset(nuster.nosql, 0, sizeof(*nuster.nosql));

        if(global.nuster.nosql.root) {
            nuster.nosql->disk.file = nst_nosql_memory_alloc(
                    nst_persist_path_file_len(global.nuster.nosql.root) + 1);

            if(!nuster.nosql->disk.file) {
                goto err;
            }
        }

        if(nst_shctx_init(nuster.nosql) != NST_OK) {
            goto shm_err;
        }

        if(nst_nosql_dict_init() != NST_OK) {
            goto err;
        }

        if(nst_nosql_stats_init() != NST_OK) {
            goto err;
        }

    }

    return;

err:
    ha_alert("Out of memory when initializing nuster nosql.\n");
    exit(1);

shm_err:
    ha_alert("Error when initializing nuster nosql memory.\n");
    exit(1);
}

static int htx_reply_100_continue(struct stream *s) {
    struct channel *res = &s->res;
    struct htx *htx = htx_from_buf(&res->buf);
    struct htx_sl *sl;
    unsigned int flags = (HTX_SL_F_IS_RESP|HTX_SL_F_VER_11|
            HTX_SL_F_XFER_LEN|HTX_SL_F_BODYLESS);
    size_t data;

    sl = htx_add_stline(htx, HTX_BLK_RES_SL, flags,
            ist("HTTP/1.1"), ist("100"), ist("Continue"));
    if (!sl)
        goto fail;
    sl->info.res.status = 100;

    if (!htx_add_endof(htx, HTX_BLK_EOH))
        goto fail;

    data = htx->data - co_data(res);
    c_adv(res, data);
    res->total += data;
    return 0;

fail:
    /* If an error occurred, remove the incomplete HTTP response from the
     * buffer */
    channel_htx_truncate(res, htx);
    return -1;
}

static int htx_handle_expect_hdr
(struct stream *s, struct htx *htx, struct http_msg *msg) {
    /* If we have HTTP/1.1 message with a body and Expect: 100-continue,
     * then we must send an HTTP/1.1 100 Continue intermediate response.
     */
    if (msg->msg_state == HTTP_MSG_BODY && (msg->flags & HTTP_MSGF_VER_11) &&
            (msg->flags & (HTTP_MSGF_CNT_LEN|HTTP_MSGF_TE_CHNK))) {
        struct ist hdr = { .ptr = "Expect", .len = 6 };
        struct http_hdr_ctx ctx;

        ctx.blk = NULL;
        /* Expect is allowed in 1.1, look for it */
        if (http_find_header(htx, hdr, &ctx, 0) &&
                unlikely(isteqi(ctx.value, ist2("100-continue", 12)))) {
            if (htx_reply_100_continue(s) == -1)
                return -1;
            http_remove_header(htx, &ctx);
        }
    }
    return 0;
}

/*
 * return 1 if the request is done, otherwise 0
 */
int nst_nosql_check_applet(struct stream *s, struct channel *req,
        struct proxy *px) {

    if(global.nuster.nosql.status == NST_STATUS_ON
            && px->nuster.mode == NST_MODE_NOSQL) {

        struct stream_interface *si = &s->si[1];
        struct http_txn *txn        = s->txn;
        struct http_msg *msg        = &txn->req;
        struct appctx *appctx       = NULL;
        struct htx *htx;

        s->target = &nuster.applet.nosql_engine.obj_type;

        if(unlikely(!si_register_handler(si, objt_applet(s->target)))) {
            txn->status = 500;
            nst_response(s, &nst_http_msg_chunks[NST_HTTP_500]);
            return 1;
        } else {
            appctx      = si_appctx(si);
            appctx->st0 = NST_NOSQL_APPCTX_STATE_INIT;
            appctx->st1 = 0;
            appctx->st2 = 0;

            htx = htxbuf(&req->buf);

            if(htx_handle_expect_hdr(s, htx, msg) == -1) {
                return 1;
            }

            req->analysers &=
                (AN_REQ_HTTP_BODY | AN_REQ_FLT_HTTP_HDRS | AN_REQ_FLT_END);

            req->analysers &= ~AN_REQ_FLT_XFER_DATA;
            req->analysers |= AN_REQ_HTTP_XFER_BODY;

        }
    }

    return 0;
}

int nst_nosql_prebuild_key(struct nst_nosql_ctx *ctx, struct stream *s,
        struct http_msg *msg) {

    struct htx *htx = htxbuf(&s->req.buf);
    struct http_hdr_ctx hdr = { .blk = NULL };

    struct htx_sl *sl;
    struct ist uri;
    char *uri_begin, *uri_end;
    char *ptr;

    ctx->req.scheme = SCH_HTTP;
#ifdef USE_OPENSSL
    if(s->sess->listener->bind_conf->is_ssl) {
        ctx->req.scheme = SCH_HTTPS;
    }
#endif

    ctx->req.host.data = NULL;
    ctx->req.host.len  = 0;

    if(http_find_header(htx, ist("Host"), &hdr, 0)) {
        ctx->req.host.data = nst_nosql_memory_alloc(hdr.value.len);

        if(!ctx->req.host.data) {
            return 0;
        }

        ctx->req.host.len  = hdr.value.len;
        memcpy(ctx->req.host.data, hdr.value.ptr, hdr.value.len);
    }

    sl = http_get_stline(htx);
    uri = htx_sl_req_uri(sl);

    if (!uri.len || *uri.ptr != '/') {
        return NST_ERR;
    }

    uri_begin = uri.ptr;
    uri_end   = uri.ptr + uri.len;

    ctx->req.path.data = NULL;
    ctx->req.path.len  = 0;
    ctx->req.uri.data  = NULL;
    ctx->req.uri.len   = 0;

    ptr = uri_begin;

    while(ptr < uri_end && *ptr != '?') {
        ptr++;
    }

    ctx->req.path.len = ptr - uri_begin;
    ctx->req.uri.data = uri_begin;
    ctx->req.uri.len  = uri.len;

    /* extra 1 char as required by regex_exec_match2 */
    ctx->req.path.data = nst_nosql_memory_alloc(ctx->req.path.len + 1);

    if(!ctx->req.path.data) {
        return 0;
    }

    memcpy(ctx->req.path.data, uri_begin, ctx->req.path.len);

    ctx->req.query.data = NULL;
    ctx->req.query.len  = 0;
    ctx->req.delimiter  = 0;

    if(ctx->req.uri.data) {
        ctx->req.query.data = memchr(ctx->req.uri.data, '?',
                uri_end - ctx->req.uri.data);

        if(ctx->req.query.data) {
            ctx->req.query.data++;
            ctx->req.query.len = uri_end - ctx->req.query.data;

            if(ctx->req.query.len) {
                ctx->req.delimiter = 1;
            }
        }
    }

    ctx->req.cookie.data = NULL;
    ctx->req.cookie.len  = 0;

    if(http_find_header(htx, ist("Cookie"), &hdr, 1)) {
        ctx->req.cookie.data = hdr.value.ptr;
        ctx->req.cookie.len  = hdr.value.len;
    }

    ctx->req.transfer_encoding.data = NULL;
    ctx->req.transfer_encoding.len  = 0;
    ctx->req.content_type.data      = NULL;
    ctx->req.content_type.len       = 0;

    return 1;
}

int nst_nosql_build_key(struct nst_nosql_ctx *ctx, struct nst_rule_key **pck,
        struct stream *s, struct http_msg *msg) {

    struct http_txn *txn = s->txn;

    struct nst_rule_key *ck = NULL;

    ctx->key  = nst_nosql_key_init();

    if(!ctx->key) {
        return NST_ERR;
    }

    nst_debug(s, "[nosql] Calculate key: ");

    while((ck = *pck++)) {
        int ret = NST_OK;

        switch(ck->type) {
            case NST_RULE_KEY_METHOD:
                nst_debug2("method.");
                ret = nst_nosql_key_append(ctx->key,
                        http_known_methods[HTTP_METH_GET].ptr,
                        http_known_methods[HTTP_METH_GET].len);
                break;
            case NST_RULE_KEY_SCHEME:
                nst_debug2("scheme.");
                ret = nst_nosql_key_append(ctx->key,
                        ctx->req.scheme == SCH_HTTPS ? "HTTPS" : "HTTP",
                        ctx->req.scheme == SCH_HTTPS ? 5 : 4);
                break;
            case NST_RULE_KEY_HOST:
                nst_debug2("host.");

                if(ctx->req.host.data) {
                    ret = nst_nosql_key_append(ctx->key, ctx->req.host.data,
                            ctx->req.host.len);
                } else {
                    ret = nst_nosql_key_advance(ctx->key, 2);
                }

                break;
            case NST_RULE_KEY_URI:
                nst_debug2("uri.");

                if(ctx->req.uri.data) {
                    ret = nst_nosql_key_append(ctx->key, ctx->req.uri.data,
                            ctx->req.uri.len);

                } else {
                    ret = nst_nosql_key_advance(ctx->key, 2);
                }

                break;
            case NST_RULE_KEY_PATH:
                nst_debug2("path.");

                if(ctx->req.path.data) {
                    ret = nst_nosql_key_append(ctx->key, ctx->req.path.data,
                            ctx->req.path.len);

                } else {
                    ret = nst_nosql_key_advance(ctx->key, 2);
                }

                break;
            case NST_RULE_KEY_DELIMITER:
                nst_debug2("delimiter.");

                if(ctx->req.delimiter) {
                    ret = nst_nosql_key_append(ctx->key, "?", 1);
                } else {
                    ret = nst_nosql_key_advance(ctx->key, 2);
                }

                break;
            case NST_RULE_KEY_QUERY:
                nst_debug2("query.");

                if(ctx->req.query.data && ctx->req.query.len) {
                    ret = nst_nosql_key_append(ctx->key, ctx->req.query.data,
                            ctx->req.query.len);

                } else {
                    ret = nst_nosql_key_advance(ctx->key, 2);
                }

                break;
            case NST_RULE_KEY_PARAM:
                nst_debug2("param_%s.", ck->data);

                if(ctx->req.query.data && ctx->req.query.len) {
                    char *v = NULL;
                    int v_l = 0;

                    if(nst_req_find_param(ctx->req.query.data,
                                ctx->req.query.data + ctx->req.query.len,
                                ck->data, &v, &v_l) == NST_OK) {

                        ret = nst_nosql_key_append(ctx->key, v, v_l);
                        break;
                    }

                }

                ret = nst_nosql_key_advance(ctx->key, 2);

                break;
            case NST_RULE_KEY_HEADER:
                {
                    struct htx *htx = htxbuf(&s->req.buf);
                    struct http_hdr_ctx hdr = { .blk = NULL };
                    struct ist h = {
                        .ptr = ck->data,
                        .len = strlen(ck->data),
                    };

                    nst_debug2("header_%s.", ck->data);

                    while (http_find_header(htx, h, &hdr, 0)) {
                        ret = nst_nosql_key_append(ctx->key, hdr.value.ptr,
                                hdr.value.len);
                    }

                    ret = ret == NST_OK && nst_nosql_key_advance(ctx->key,
                            hdr.value.len == 0 ? 2 : 1);

                }
                break;
            case NST_RULE_KEY_COOKIE:
                nst_debug2("cookie_%s.", ck->data);

                if(ctx->req.cookie.data) {
                    char *v = NULL;
                    size_t v_l = 0;

                    if(http_extract_cookie_value(ctx->req.cookie.data,
                                ctx->req.cookie.data + ctx->req.cookie.len,
                                ck->data, strlen(ck->data), 1, &v, &v_l)) {

                        ret = nst_nosql_key_append(ctx->key, v, v_l);
                        break;
                    }

                }

                break;
            case NST_RULE_KEY_BODY:
                nst_debug2("body.");

                if(txn->meth == HTTP_METH_POST || txn->meth == HTTP_METH_PUT) {

                    //if((s->be->options & PR_O_WREQ_BODY)
                    //        && ci_data(msg->chn) - msg->sov > 0) {

                    //    ret = nst_nosql_key_append(ctx->key,
                    //            ci_head(msg->chn) + msg->sov,
                    //            ci_data(msg->chn) - msg->sov);

                    //} else {
                    //    ret = nst_nosql_key_advance(ctx->key, 2);
                    //}
                }

                break;
            default:
                ret = NST_ERR;
                break;
        }

        if(ret != NST_OK) {
            return NST_ERR;
        }
    }

    nst_debug2("\n");

    return NST_OK;
}

void nst_nosql_hit(struct stream *s, struct stream_interface *si,
        struct channel *req, struct channel *res, struct nst_nosql_data *data) {
}

int nst_nosql_get_headers(struct nst_nosql_ctx *ctx, struct stream *s,
        struct http_msg *msg) {

    struct htx *htx = htxbuf(&s->req.buf);
    struct http_hdr_ctx hdr = { .blk = NULL };

    if(http_find_header(htx, ist("Content-Type"), &hdr, 0)) {

        ctx->req.content_type.data = nst_nosql_memory_alloc(hdr.value.len);

        if(!ctx->req.content_type.data) {
            return 0;
        }

        ctx->req.content_type.len = hdr.value.len;
        memcpy(ctx->req.content_type.data, hdr.value.ptr, hdr.value.len);
    }

    while(http_find_header(htx, ist("Transfer-Encoding"), &hdr, 0)) {

        char *p = ctx->req.transfer_encoding.data;
        int len = p
            ? ctx->req.transfer_encoding.len + hdr.value.len + 1
            : ctx->req.transfer_encoding.len + hdr.value.len;

        ctx->req.transfer_encoding.data = nst_nosql_memory_alloc(len);

        if(!ctx->req.transfer_encoding.data) {

            if(p) {
                nst_nosql_memory_free(p);
            }

            return 0;
        }

        if(p) {
            memcpy(ctx->req.transfer_encoding.data, p,
                    ctx->req.transfer_encoding.len);

            ctx->req.transfer_encoding.data[ctx->req.transfer_encoding.len] =
                ',';

            nst_nosql_memory_free(p);
            memcpy(ctx->req.transfer_encoding.data
                    + ctx->req.transfer_encoding.len + 1,
                    hdr.value.ptr, hdr.value.len);

        } else {
            memcpy(ctx->req.transfer_encoding.data, hdr.value.ptr,
                    hdr.value.len);
        }

        ctx->req.transfer_encoding.len = len;
    }

    return 1;
}

void nst_res_header_create(struct nst_nosql_ctx *ctx, struct stream *s,
        int status, struct ist ctv) {

    struct htx_sl  *sl;
    uint32_t size;
    enum htx_blk_type type;
    struct ist p1;
    struct ist p2;
    struct ist p3;
    uint32_t info;
    struct nst_nosql_element *element = NULL;
    char *data = NULL;

    p1 = ist("HTTP/1.1");
    p2 = ist("200");
    p3 = ist("OK");

    type = HTX_BLK_RES_SL;

    info = type << 28;
    size = sizeof(*sl) + p1.len + p2.len + p3.len;
    info += size;

    element = nst_nosql_memory_alloc(sizeof(*element));

    if(!element) {
        return;
    }

    data = nst_nosql_memory_alloc(size);
    ctx->header_len += 4 + size;
    ctx->cache_len2 += 4 + size;

    if(!data) {
        return;
    }

    sl = (struct htx_sl *)data;
    sl->hdrs_bytes = -1;

    if(ctx->cache_len) {
        sl->flags = (HTX_SL_F_IS_RESP | HTX_SL_F_VER_11 | HTX_SL_F_XFER_LEN
                |HTX_SL_F_CLEN);
    } else {
        sl->flags = (HTX_SL_F_IS_RESP | HTX_SL_F_VER_11 | HTX_SL_F_XFER_ENC
                | HTX_SL_F_XFER_LEN | HTX_SL_F_CHNK);
    }

    HTX_SL_P1_LEN(sl) = p1.len;
    HTX_SL_P2_LEN(sl) = p2.len;
    HTX_SL_P3_LEN(sl) = p3.len;
    memcpy(HTX_SL_P1_PTR(sl), p1.ptr, p1.len);
    memcpy(HTX_SL_P2_PTR(sl), p2.ptr, p2.len);
    memcpy(HTX_SL_P3_PTR(sl), p3.ptr, p3.len);

    element->msg.data = data;
    element->msg.len  = info;

    ctx->data->element = element;
    ctx->element = element;

    if(ctx->cache_len) {
        struct ist ctk = ist("Content-Length");

        type = HTX_BLK_HDR;
        info = type << 28;

        size = ctv.len + ctk.len;
        info += (ctv.len << 8) + ctk.len;

        element = nst_nosql_memory_alloc(sizeof(*element));

        if(!element) {
            return;
        }

        data = nst_nosql_memory_alloc(size);
        ctx->header_len += 4 + size;
        ctx->cache_len2 += 4 + size;

        if(!data) {
            return;
        }

        ist2bin_lc(data, ctk);
        memcpy(data + ctk.len, ctv.ptr, ctv.len);

        element->msg.data = data;
        element->msg.len  = info;

        ctx->element->next = element;
        ctx->element = element;
    } else if(ctx->data->info.flags & NST_NOSQL_DATA_FLAG_CHUNKED) {
        struct ist k = ist("Transfer-Encoding");
        struct ist v = ist("Chunked");

        type = HTX_BLK_HDR;
        info = type << 28;

        size = k.len + v.len;
        info += (v.len << 8) + k.len;

        element = nst_nosql_memory_alloc(sizeof(*element));

        if(!element) {
            return;
        }

        data = nst_nosql_memory_alloc(size);
        ctx->header_len += 4 + size;
        ctx->cache_len2 += 4 + size;

        if(!data) {
            return;
        }

        ist2bin_lc(data, k);
        memcpy(data + k.len, v.ptr, v.len);

        element->msg.data = data;
        element->msg.len  = info;

        ctx->element->next = element;
        ctx->element = element;
    }

    type = HTX_BLK_EOH;

    info = type << 28;
    size = 1;
    info += size;

    element = nst_nosql_memory_alloc(sizeof(*element));

    if(!element) {
        return;
    }

    data = nst_nosql_memory_alloc(size);
    ctx->header_len += 4 + size;

    if(!data) {
        return;
    }

    element->msg.data = data;
    element->msg.len  = info;

    ctx->element->next = element;
    ctx->element = element;

}

void nst_nosql_create(struct nst_nosql_ctx *ctx, struct stream *s,
        struct http_msg *msg) {

    struct nst_nosql_entry *entry = NULL;
    struct nst_nosql_element *element = NULL;

    /* Check if nosql is full */
    if(nst_nosql_stats_full()) {
        ctx->state = NST_NOSQL_CTX_STATE_FULL;
        return;
    }

    nst_shctx_lock(&nuster.nosql->dict[0]);
    entry = nst_nosql_dict_get(ctx->key, ctx->hash);

    if(entry) {

        if(entry->state == NST_NOSQL_ENTRY_STATE_CREATING) {
            ctx->state = NST_NOSQL_CTX_STATE_WAIT;
        } else {
            entry->state = NST_NOSQL_ENTRY_STATE_CREATING;

            if(entry->data) {
                entry->data->invalid = 1;
            }

            entry->data = nst_nosql_data_new();
            ctx->state  = NST_NOSQL_CTX_STATE_CREATE;
        }
    } else {
        ctx->state = NST_NOSQL_CTX_STATE_CREATE;
        entry = nst_nosql_dict_set(ctx);
    }

    nst_shctx_unlock(&nuster.nosql->dict[0]);

    if(!entry || !entry->data) {
        ctx->state   = NST_NOSQL_CTX_STATE_INVALID;
    } else {

        if(ctx->state == NST_NOSQL_CTX_STATE_CREATE) {
            struct htx *htx = htxbuf(&msg->chn->buf);
            struct htx_sl *sl;
            struct http_hdr_ctx hdr = { .blk = NULL };

            ctx->entry   = entry;
            ctx->data    = entry->data;
            ctx->element = entry->data->element;

            sl = http_get_stline(htx);

            if(sl->flags & HTX_SL_F_CLEN) {
                if(http_find_header(htx, ist("Content-Length"), &hdr, 0)) {
                    long long cl;
                    strl2llrc(hdr.value.ptr, hdr.value.len, &cl);
                    ctx->cache_len = cl;
                }
            }

            if(sl->flags & HTX_SL_F_CHNK) {
                entry->data->info.flags = NST_NOSQL_DATA_FLAG_CHUNKED;
            }

            nst_res_header_create(ctx, s, 200, hdr.value);

        }
    }


    if(ctx->state == NST_NOSQL_CTX_STATE_CREATE
            && (ctx->rule->disk == NST_DISK_SYNC
                || ctx->rule->disk == NST_DISK_ONLY)) {

        ctx->disk.file = nst_nosql_memory_alloc(
                nst_persist_path_file_len(global.nuster.nosql.root) + 1);

        if(!ctx->disk.file) {
            return;
        }

        if(nst_persist_init(global.nuster.nosql.root, ctx->disk.file,
                    ctx->hash) != NST_OK) {

            return;
        }

        ctx->disk.fd = nst_persist_create(ctx->disk.file);

        nst_persist_meta_init(ctx->disk.meta, (char)ctx->rule->disk, ctx->hash,
                0, 0, ctx->header_len, ctx->entry->key->data, 0, 0, 0, 0, 0);

        nst_persist_write_key(&ctx->disk, ctx->entry->key);

        ctx->disk.offset = NST_PERSIST_META_SIZE + ctx->entry->key->data;

        element = ctx->data->element;

        while(element) {
            int sz = ((element->msg.len & 0xff)
                    + ((element->msg.len >> 8) & 0xfffff));

            nst_persist_write(&ctx->disk, (char *)&element->msg.len, 4);
            nst_persist_write(&ctx->disk, element->msg.data, sz);

            element = element->next;
        }

    }

err:
    return;
}

int nst_nosql_update(struct nst_nosql_ctx *ctx, struct http_msg *msg,
        unsigned int offset, unsigned int msg_len) {

    int pos;
    struct htx *htx = htxbuf(&msg->chn->buf);

    for (pos = htx_get_first(htx); pos != -1; pos = htx_get_next(htx, pos)) {
        struct htx_blk *blk = htx_get_blk(htx, pos);
        uint32_t        sz  = htx_get_blksz(blk);
        enum htx_blk_type type = htx_get_blk_type(blk);
        struct nst_nosql_element *element;
        char *data;

        if(type != HTX_BLK_DATA) {
            continue;
        }

        if(ctx->rule->disk == NST_DISK_ONLY)  {
            nst_persist_write(&ctx->disk, htx_get_blk_ptr(htx, blk), sz);
            ctx->cache_len2 += sz;
        } else {
            element = nst_nosql_memory_alloc(sizeof(*element));

            if(!element) {
                goto err;
            }

            data = nst_nosql_memory_alloc(sz);

            if(!data) {
                goto err;
            }

            memcpy(data, htx_get_blk_ptr(htx, blk), sz);

            element->msg.data = data;
            element->msg.len  = blk->info;

            if(ctx->element) {
                ctx->element->next = element;
            } else {
                ctx->data->element = element;
            }

            ctx->element = element;

            if(ctx->rule->disk == NST_DISK_SYNC) {
                nst_persist_write(&ctx->disk, htx_get_blk_ptr(htx, blk), sz);
            }

            ctx->cache_len2 += sz;

        }
    }

    return NST_OK;

err:

    return NST_ERR;
}

int nst_nosql_exists(struct nst_nosql_ctx *ctx, int mode) {
    struct nst_nosql_entry *entry = NULL;
    int ret = NST_CACHE_CTX_STATE_INIT;

    if(!ctx->key) {
        return ret;
    }

    nst_shctx_lock(&nuster.nosql->dict[0]);
    entry = nst_nosql_dict_get(ctx->key, ctx->hash);

    if(entry) {
        if(entry->state == NST_NOSQL_ENTRY_STATE_VALID) {
            ctx->data = entry->data;
            ctx->data->clients++;
            ret = NST_NOSQL_CTX_STATE_HIT;
        }

        if(entry->state == NST_NOSQL_ENTRY_STATE_INVALID && entry->file) {
            ctx->disk.file = entry->file;
            ret = NST_NOSQL_CTX_STATE_CHECK_PERSIST;
        }
    } else {
        if(mode != NST_DISK_OFF) {
            ctx->disk.file = NULL;
            //if(nuster.nosql->disk.loaded) {
            //    ret = NST_NOSQL_CTX_STATE_INIT;
            //} else {
                ret = NST_NOSQL_CTX_STATE_CHECK_PERSIST;
            //}
        }
    }

    nst_shctx_unlock(&nuster.nosql->dict[0]);

    if(ret == NST_NOSQL_CTX_STATE_CHECK_PERSIST) {
        if(ctx->disk.file) {
            if(nst_persist_valid(&ctx->disk, ctx->key, ctx->hash) == NST_OK) {

                ret = NST_NOSQL_CTX_STATE_HIT_DISK;
            } else {
                ret = NST_NOSQL_CTX_STATE_INIT;
            }
        } else {
            ctx->disk.file = nst_nosql_memory_alloc(
                    nst_persist_path_file_len(global.nuster.nosql.root) + 1);

            if(!ctx->disk.file) {
                ret = NST_NOSQL_CTX_STATE_INIT;
            } else {

                if(nst_persist_exists(global.nuster.nosql.root, &ctx->disk,
                            ctx->key, ctx->hash) == NST_OK) {

                    ret = NST_NOSQL_CTX_STATE_HIT_DISK;
                } else {
                    nst_nosql_memory_free(ctx->disk.file);
                    ret = NST_NOSQL_CTX_STATE_INIT;
                }
            }
        }
    }

    return ret;
}

int nst_nosql_delete(struct buffer *key, uint64_t hash) {
    struct nst_nosql_entry *entry = NULL;
    int ret = 0;

    if(!key) {
        return 0;
    }

    nst_shctx_lock(&nuster.nosql->dict[0]);
    entry = nst_nosql_dict_get(key, hash);

    if(entry) {
        entry->state = NST_NOSQL_ENTRY_STATE_INVALID;
        ret = 1;
    }

    nst_shctx_unlock(&nuster.nosql->dict[0]);

    return ret;
}

void nst_nosql_finish(struct nst_nosql_ctx *ctx, struct stream *s,
        struct http_msg *msg) {

    if(ctx->cache_len == 0 && ctx->cache_len2 == 0) {
        ctx->state = NST_NOSQL_CTX_STATE_INVALID;
        ctx->entry->state = NST_NOSQL_ENTRY_STATE_INVALID;
    } else {

        if(ctx->req.content_type.data) {
            ctx->entry->data->info.content_type.data =
                ctx->req.content_type.data;

            ctx->entry->data->info.content_type.len  =
                ctx->req.content_type.len;

            ctx->req.content_type.data = NULL;
        }

        if(ctx->req.transfer_encoding.data) {
            ctx->entry->data->info.transfer_encoding.data =
                ctx->req.transfer_encoding.data;

            ctx->entry->data->info.transfer_encoding.len  =
                ctx->req.transfer_encoding.len;

            ctx->req.transfer_encoding.data = NULL;
        }

        if(ctx->cache_len) {
            ctx->entry->data->info.content_length = ctx->cache_len;
        } else {
            ctx->entry->data->info.content_length = ctx->cache_len2;
        }

        if(msg->flags & HTTP_MSGF_TE_CHNK) {
            ctx->entry->data->info.flags = NST_NOSQL_DATA_FLAG_CHUNKED;
        } else {
            ctx->entry->data->info.flags = 0;
        }

        ctx->state = NST_NOSQL_CTX_STATE_DONE;

        if(ctx->rule->disk == NST_DISK_ONLY) {
            ctx->entry->state = NST_NOSQL_ENTRY_STATE_INVALID;
        } else {
            ctx->entry->state = NST_NOSQL_ENTRY_STATE_VALID;
        }


        if(*ctx->rule->ttl == 0) {
            ctx->entry->expire = 0;
        } else {
            ctx->entry->expire = get_current_timestamp() / 1000
                + *ctx->rule->ttl;
        }

        if(ctx->rule->disk == NST_DISK_SYNC
                || ctx->rule->disk == NST_DISK_ONLY) {

            nst_persist_meta_set_expire(ctx->disk.meta, ctx->entry->expire);

            if(ctx->cache_len) {
                nst_persist_meta_set_cache_len(ctx->disk.meta, ctx->cache_len);
            } else {
                nst_persist_meta_set_cache_len(ctx->disk.meta, ctx->cache_len2);
            }

            nst_persist_write_meta(&ctx->disk);

            ctx->entry->file = ctx->disk.file;
        }
    }
}

void nst_nosql_abort(struct nst_nosql_ctx *ctx) {
    ctx->entry->state = NST_NOSQL_ENTRY_STATE_INVALID;
}

void nst_nosql_persist_async() {
    struct nst_nosql_entry *entry;

    if(!global.nuster.nosql.root || !nuster.nosql->disk.loaded) {
        return;
    }

    if(!nuster.nosql->dict[0].used) {
        return;
    }

    entry = nuster.nosql->dict[0].entry[nuster.nosql->persist_idx];

    while(entry) {

        if(entry->state == NST_NOSQL_ENTRY_STATE_VALID
                && entry->rule->disk == NST_DISK_ASYNC
                && entry->file == NULL) {

            struct nst_nosql_element *element = entry->data->element;
            uint64_t cache_len = 0;
            struct persist disk;
            uint64_t header_len = 0;

            entry->file = nst_nosql_memory_alloc(
                    nst_persist_path_file_len(global.nuster.nosql.root) + 1);

            if(!entry->file) {
                return;
            }

            if(nst_persist_init(global.nuster.nosql.root, entry->file,
                        entry->hash) != NST_OK) {
                return;
            }

            disk.fd = nst_persist_create(entry->file);

            nst_persist_meta_init(disk.meta, (char)entry->rule->disk,
                    entry->hash, entry->expire, 0, 0,
                    entry->key->data, 0, 0, 0, 0, 0);

            nst_persist_write_key(&disk, entry->key);

            while(element) {
                uint32_t blksz, info;
                enum htx_blk_type type;

                info = element->msg.len;
                type = (info >> 28);
                blksz = ((type == HTX_BLK_HDR || type == HTX_BLK_TLR)
                        ? (info & 0xff) + ((info >> 8) & 0xfffff)
                        : info & 0xfffffff);

                if(type != HTX_BLK_DATA) {
                    nst_persist_write(&disk, (char *)&info, 4);
                    cache_len += 4;
                    header_len += 4 + blksz;
                }

                nst_persist_write(&disk, element->msg.data, blksz);

                cache_len += blksz;

                element = element->next;
            }

            nst_persist_meta_set_header_len(disk.meta, header_len);
            nst_persist_meta_set_cache_len(disk.meta, cache_len);

            nst_persist_write_meta(&disk);

            close(disk.fd);
        }

        entry = entry->next;

    }

    nuster.nosql->persist_idx++;

    /* if we have checked the whole dict */
    if(nuster.nosql->persist_idx == nuster.nosql->dict[0].size) {
        nuster.nosql->persist_idx = 0;
    }

}

void nst_nosql_persist_load() {

    if(global.nuster.nosql.root && !nuster.nosql->disk.loaded) {
        char *root;
        char *file;
        char meta[NST_PERSIST_META_SIZE];
        struct buffer *key;
        int fd;

        root = global.nuster.nosql.root;
        file = nuster.nosql->disk.file;

        if(nuster.nosql->disk.dir) {
            struct dirent *de = nst_persist_dir_next(nuster.nosql->disk.dir);

            if(de) {
                DIR *dir2;
                struct dirent *de2;

                if(strcmp(de->d_name, ".") == 0
                        || strcmp(de->d_name, "..") == 0) {

                    return;
                }

                memcpy(file + nst_persist_path_base_len(root), "/", 1);
                memcpy(file + nst_persist_path_base_len(root) + 1, de->d_name,
                        strlen(de->d_name));

                dir2 = opendir(file);

                if(!dir2) {
                    return;
                }

                while((de2 = readdir(dir2)) != NULL) {
                    if(strcmp(de2->d_name, ".") == 0
                            || strcmp(de2->d_name, "..") == 0) {

                        continue;
                    }

                    memcpy(file + nst_persist_path_hash_len(root), "/", 1);
                    memcpy(file + nst_persist_path_hash_len(root) + 1,
                            de2->d_name, strlen(de2->d_name));

                    fd = nst_persist_open(file);

                    if(fd == -1) {
                        closedir(dir2);
                        return;
                    }

                    if(nst_persist_get_meta(fd, meta) != NST_OK) {
                        unlink(file);
                        close(fd);
                        closedir(dir2);
                        return;
                    }

                    key = nst_nosql_memory_alloc(sizeof(*key));

                    if(!key) {
                        unlink(file);
                        close(fd);
                        closedir(dir2);
                        return;
                    }

                    key->size = nst_persist_meta_get_key_len(meta);
                    key->area = nst_nosql_memory_alloc(key->size);

                    if(!key->area) {
                        nst_nosql_memory_free(key);
                        unlink(file);
                        close(fd);
                        closedir(dir2);
                        return;
                    }

                    if(nst_persist_get_key(fd, meta, key) != NST_OK) {
                        nst_nosql_memory_free(key->area);

                        nst_nosql_memory_free(key);

                        unlink(file);
                        close(fd);
                        closedir(dir2);
                        return;
                    }

                    nst_nosql_dict_set_from_disk(file, meta, key);

                    close(fd);
                }

                closedir(dir2);
            } else {
                nuster.nosql->disk.idx++;
                closedir(nuster.nosql->disk.dir);
                nuster.nosql->disk.dir = NULL;
            }
        } else {
            nuster.nosql->disk.dir = nst_persist_opendir_by_idx(
                    global.nuster.nosql.root, file, nuster.nosql->disk.idx);

            if(!nuster.nosql->disk.dir) {
                nuster.nosql->disk.idx++;
            }
        }

        if(nuster.nosql->disk.idx == 16 * 16) {
            nuster.nosql->disk.loaded = 1;
            nuster.nosql->disk.idx    = 0;
        }

    }
}

void nst_nosql_persist_cleanup() {

    if(global.nuster.nosql.root && nuster.nosql->disk.loaded) {
        char *file = nuster.nosql->disk.file;

        if(nuster.nosql->disk.dir) {
            struct dirent *de = nst_persist_dir_next(nuster.nosql->disk.dir);

            if(de) {
                nst_persist_cleanup(global.nuster.nosql.root, file, de);
            } else {
                nuster.nosql->disk.idx++;
                closedir(nuster.nosql->disk.dir);
                nuster.nosql->disk.dir = NULL;
            }
        } else {
            nuster.nosql->disk.dir = nst_persist_opendir_by_idx(
                    global.nuster.nosql.root, file, nuster.nosql->disk.idx);

            if(!nuster.nosql->disk.dir) {
                nuster.nosql->disk.idx++;
            }
        }

        if(nuster.nosql->disk.idx == 16 * 16) {
            nuster.nosql->disk.idx = 0;
        }

    }
}
