#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_http.h>

#include "ngx_http_handoff_module.h"
#include "connect.h"

ngx_int_t connect_to_upstream(ngx_http_request_t *r,
                              struct handoff_out *handoff_out_ctx,
                              ngx_event_handler_pt connect_handler,
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
    upstream_conn = ngx_get_connection(s, r->connection->log);
    assert(upstream_conn != NULL);
    ngx_reusable_connection(upstream_conn, 1);
    *conn = upstream_conn;
    upstream_conn->type = type;
    upstream_conn->data = r;
    upstream_conn->handoff_out_ctx = handoff_out_ctx;
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
    upstream_conn->log = r->connection->log;
    upstream_conn->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
    upstream_conn->start_time = ngx_current_msec;

    rev = ngx_calloc(sizeof(ngx_event_t), upstream_conn->log);
    wev = ngx_calloc(sizeof(ngx_event_t), upstream_conn->log);

    rev->handler = connect_handler;
    wev->handler = connect_handler;
    upstream_conn->read = rev;
    upstream_conn->write = wev;

    rev->log = upstream_conn->log;
    wev->log = upstream_conn->log;

    rev->data = upstream_conn;
    wev->data = upstream_conn;

    struct sockaddr_in sockaddr;
    if (handoff_out_ctx->client->to_migrate != -1) {
        // handoff from frontend
        memcpy(&sockaddr, &handoff_out_ctx->ngx_conf->peer_sockaddr[handoff_out_ctx->client->to_migrate], sizeof(struct sockaddr_in));
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, r->connection->log, 0, "Connecting back to backend");
    }
    else {
        // handoff back to frontend
        memcpy(&sockaddr, &r->connection->handoff_in_ctx->frontend_sockaddr, sizeof(struct sockaddr_in));
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, r->connection->log, 0, "Connecting back to frontend %s", inet_ntoa(sockaddr.sin_addr));
    }

    if (ngx_add_conn) {
        rc = ngx_add_conn(upstream_conn);
        assert(rc != NGX_ERROR);
    }
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, r->connection->log, 0,
                   "connect to upstream peer %d, fd:%d #%uA", upstream_conn->handoff_out_ctx->client->to_migrate, upstream_conn->number);

    rc = connect(s, (struct sockaddr*)&sockaddr, sizeof(sockaddr));
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

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, r->connection->log, 0, "TEST!!!!!!!!!!!!!! %d", rc);
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

    return NGX_OK;
}
