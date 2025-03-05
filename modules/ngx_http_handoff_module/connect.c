#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_http.h>

#include "handoff.h"
#include "ngx_http_handoff_module.h"
#include "connect.h"

ngx_int_t connect_to_upstream(struct sockaddr_in *sockaddr,
                              struct handoff_out *handoff_out_ctx,
                              size_t pool_size,
                              ngx_event_handler_pt connect_handler,
                              ngx_log_t *log,
                              ngx_connection_t **conn) {
    int              rc, type, value;
//    in_port_t        port;
    ngx_int_t          event;
    ngx_socket_t     s;
    ngx_event_t      *rev, *wev;
    ngx_connection_t *upstream_conn;

    type = SOCK_STREAM;
    s = ngx_socket(AF_INET, type, IPPROTO_TCP);
    assert(s != -1);
    upstream_conn = ngx_get_connection(s, log);
    if (upstream_conn == NULL) {
        if (ngx_close_socket(s) == -1) {
            ngx_log_error(NGX_LOG_ALERT, log, ngx_socket_errno,
                          ngx_close_socket_n " upstream socket failed");
        }
    
        return NGX_ERROR;
    }

    ngx_reusable_connection(upstream_conn, 0);
    *conn = upstream_conn;
    upstream_conn->type = type;
    //upstream_conn->data = r;

    upstream_conn->pool = ngx_create_pool(pool_size, log);
    assert(upstream_conn->pool != NULL);

    //upstream_conn->log = ngx_pcalloc(upstream_conn->pool, sizeof(ngx_log_t));
    upstream_conn->log = calloc(1, sizeof(ngx_log_t));
    *(upstream_conn->log) = *log;
    upstream_conn->pool->log = upstream_conn->log;

    // ownership of this handoff_out_ctx should be the control conn to upstream
    //upstream_conn->handoff_out_ctx = ngx_pcalloc(upstream_conn->pool, sizeof(struct handoff_out));
    upstream_conn->handoff_out_ctx = calloc(1, sizeof(struct handoff_out));
    memcpy(upstream_conn->handoff_out_ctx, handoff_out_ctx, sizeof(struct handoff_out));
    memcpy(upstream_conn->handoff_out_ctx->client, handoff_out_ctx->client, sizeof(struct http_client));

    value = 1;
    rc = setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const void *) &value, sizeof(int));
    assert(rc == 0);
    rc = ngx_nonblocking(s);
    assert(rc == 0);

    // port 79
    upstream_conn->recv = ngx_recv;
    upstream_conn->send = ngx_send;
    upstream_conn->recv_chain = ngx_recv_chain;
    upstream_conn->send_chain = ngx_send_chain;

    //upstream_conn->data = r;

    upstream_conn->sendfile = 1;
    upstream_conn->log = log;
    upstream_conn->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
    upstream_conn->start_time = ngx_current_msec;

    //rev = ngx_calloc(sizeof(ngx_event_t), upstream_conn->log);
    //wev = ngx_calloc(sizeof(ngx_event_t), upstream_conn->log);
    rev = upstream_conn->read;
    wev = upstream_conn->write;

    rev->handler = connect_handler;
    wev->handler = connect_handler;
    upstream_conn->read = rev;
    upstream_conn->write = wev;

    rev->log = upstream_conn->log;
    wev->log = upstream_conn->log;

    rev->data = upstream_conn;
    wev->data = upstream_conn;

//    if (handoff_out_ctx->client->to_migrate != -1) {
//        // handoff from frontend
//        //memcpy(&sockaddr, &handoff_out_ctx->ngx_conf->peer_sockaddr[handoff_out_ctx->client->to_migrate], sizeof(struct sockaddr_in));
//        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, log, 0, "Connecting back to backend");
//    }
//    else {
//        // handoff back to frontend
//        //memcpy(&sockaddr, &r->connection->handoff_in_ctx->frontend_sockaddr, sizeof(struct sockaddr_in));
//        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, log, 0, "Connecting back to frontend %s", inet_ntoa(sockaddr.sin_addr));
//    }

    if (ngx_add_conn) {
        rc = ngx_add_conn(upstream_conn);
        assert(rc != NGX_ERROR);
    }
    //ngx_log_debug4(NGX_LOG_DEBUG_EVENT, log, 0,
    //               "connect to upstream peer %d (%s:%d), fd:%d #%uA", upstream_conn->handoff_out_ctx->client->to_migrate, inet_ntoa(sockaddr->sin_addr), ntohs(sockaddr->sin_port), upstream_conn->number);

    rc = connect(s, (struct sockaddr*)sockaddr, sizeof(struct sockaddr));
    if (rc == -1 && ngx_socket_errno != NGX_EINPROGRESS) {
        ngx_log_error(NGX_LOG_ERR, upstream_conn->log, ngx_socket_errno, "connect() to %s failed",
                      "upstream");
        ngx_close_connection(upstream_conn);
        return NGX_DECLINED;
    }
    if (ngx_add_conn) {
        if (rc == -1) {
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, upstream_conn->log, 0, "connecting in progress");
            return NGX_AGAIN;
        }
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, upstream_conn->log, 0, "connected");
        wev->ready = 1;
        return NGX_OK;
    }

    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {
        /* select, poll, /dev/poll */
        event = NGX_LEVEL_EVENT;
    }

    if (ngx_add_event(rev, NGX_READ_EVENT, event) != NGX_OK) {
        ngx_close_connection(upstream_conn);
        return NGX_ERROR;
    }

    if (rc == -1) {
        /* NGX_EINPROGRESS */
        if (ngx_add_event(wev, NGX_WRITE_EVENT, event) != NGX_OK) {
            ngx_close_connection(upstream_conn);
            return NGX_ERROR;
        }

        return NGX_AGAIN;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, upstream_conn->log, 0, "connected");

    wev->ready = 1;
//    ngx_http_request_t *original_r = (ngx_http_request_t*)handoff_out_ctx->req_to_free;
//    ngx_http_finalize_request(original_r, NGX_ERROR);

    return NGX_OK;
}
