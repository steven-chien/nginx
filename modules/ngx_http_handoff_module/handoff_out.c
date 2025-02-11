#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_http.h>

#include "ngx_http_handoff_module.h"
#include "handoff_out.h"
#include "connect.h"
#include "util.h"

static int my_random(int min, int max){
   return min + rand() / (RAND_MAX / (max - min + 1) + 1);
}

static void handoff_out_read_handler(ngx_event_t *ev) {
    int rc = -1;
    ngx_connection_t *c = ev->data;
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "upstream sock read event fd=%d", c->fd);

    rc = c->recv(c, c->recv_buffer, ev->available);
    if (rc == NGX_EAGAIN) {
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "upstream sock read event fd=%d socket not ready", c->fd);
        rc = ngx_add_event(ev, NGX_READ_EVENT, NGX_LEVEL_EVENT); 
        assert(rc == 0);
    }
    else {
        if (rc == NGX_ERROR || ev->pending_eof) {
            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0, "upstream sock read event fd=%d error %s, exiting", c->fd, strerror(ngx_errno));
            rc = ngx_del_event(ev, NGX_READ_EVENT, NGX_CLEAR_EVENT); 
            ngx_close_connection(c);
            return;
        }

        // block socket until send
        ngx_blocking(c->fd);

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0, "upstream sock read event fd=%d received=%d: %s", c->fd, rc, c->recv_buffer);
        rc = ngx_del_event(ev, NGX_READ_EVENT, NGX_CLEAR_EVENT); 
        assert(rc == 0);

        // apply redirection
        struct handoff_out *handoff_out_ctx = c->handoff_out_ctx;
        ngx_http_handoff_main_conf_t *my_conf = handoff_out_ctx->ngx_conf;

        char *ptr5;
        ptr5 = strstr((char*)c->recv_buffer, "\r\n\r\n") + 4;

        char *content_len_str = strstr((char*)c->recv_buffer, "Content-Length: ") + strlen("Content-Length: ");
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "http line %s", content_len_str);
        handoff_out_ctx->recv_protobuf_received = atoi(content_len_str);
        handoff_out_ctx->recv_protobuf_len = handoff_out_ctx->recv_protobuf_received;

        //ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "content length line: %s", buf);
        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, " resp protobuf %s", ptr5+sizeof(uint32_t));

        SocketSerialize *migration_info = socket_serialize__unpack(NULL, handoff_out_ctx->recv_protobuf_len-sizeof(uint32_t), (uint8_t*)ptr5+sizeof(uint32_t));
        if (migration_info == NULL) {
                ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0, "unable to unpack protobuf len=%d %s", handoff_out_ctx->recv_protobuf_len-sizeof(uint32_t), ptr5+sizeof(uint32_t));
                exit(EXIT_FAILURE);
        }
        if (migration_info->msg_type != HANDOFF_DONE) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0, "HANDOFF_OUT Received migration respond not HANDOFF_DONE");
                exit(EXIT_FAILURE);
        }

        uint8_t fake_server_mac[6];
        memcpy(fake_server_mac, &(migration_info->peer_mac), sizeof(uint8_t) * 6);
 
        rc = apply_redirection_ebpf(migration_info->peer_addr, migration_info->self_addr,
                                    migration_info->peer_port, htons(ntohs(migration_info->self_port) - 1 - 1),
                                    migration_info->peer_addr, my_conf->my_mac, my_conf->peer_sockaddr[handoff_out_ctx->client->to_migrate].sin_addr.s_addr, fake_server_mac,
                                    migration_info->peer_port, migration_info->self_port, false);
        assert(rc == 0);

        size_t header_len = snprintf(NULL, 0, "PUT / HTTP/1.1\r\nHost: n12-cx4:79\r\nContent-Length: 5\r\nAccept: */*\r\n\r\nDONE");
        c->send_buffer_len = header_len + 1;
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0, "reply fake server redirection ready!");
        c->send_buffer = calloc(c->send_buffer_len, sizeof(uint8_t));
        snprintf((char*)c->send_buffer, c->send_buffer_len, "PUT / HTTP/1.1\r\nHost: n12-cx4:79\r\nContent-Length: 5\r\nAccept: */*\r\n\r\nDONE");

        rc = c->send(c, c->send_buffer, c->send_buffer_len);
        ngx_nonblocking(c->fd);
    }
}

static void handoff_out_write_handler(ngx_event_t *ev) {
    int rc = 0;
    size_t sent = 0;
    ngx_connection_t *c = ev->data;
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "upstream sock write event fd=%d", c->fd);

    sent = c->send(c, c->send_buffer + c->sent, c->send_buffer_len - c->sent);
    if (sent == NGX_EAGAIN || c->sent < c->send_buffer_len) {
        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0, "upstream sock write event fd=%d sent %d/%ld", c->fd, c->sent, c->send_buffer_len);
        rc = ngx_add_event(ev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT); 
        assert(rc == 0);
    }

    if (c->sent >= c->send_buffer_len) {
        free(c->send_buffer);
        c->sent = 0;
        c->send_buffer_len = 0;
        rc = ngx_del_event(ev, NGX_WRITE_EVENT, NGX_CLEAR_EVENT); 
        assert(rc == 0);
    }
}

void handoff_out_connect_handler(ngx_event_t *ev) {
    int rc = -1;
    ngx_connection_t *c = ev->data;
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0, "upstream sock connected fd=%d", c->fd);

    c->read->handler  = handoff_out_read_handler;
    c->write->handler = handoff_out_write_handler;

    if (!c->handoff_out_ctx->is_fd_connected) {
        // init handoff out
        c->handoff_out_ctx->is_fd_connected = true;
        c->handoff_out_ctx->is_fd_in_epoll = false;
        c->handoff_out_ctx->fd = c->fd;

        // send HTTP req to upstream
        size_t header_len = snprintf(NULL, 0, "PUT / HTTP/1.1\r\nHost: n12-cx4:79\r\nAccept: */*\r\nContent-length: %d\r\n\r\n", c->handoff_out_ctx->client->proto_buf_len);
        c->send_buffer_len = header_len + c->handoff_out_ctx->client->proto_buf_len;
ngx_log_debug3(NGX_LOG_DEBUG_HTTP,c->log, 0, "header length=%d protbuf len=%d total len=%d", header_len, c->handoff_out_ctx->client->proto_buf_len, c->send_buffer_len);
        c->send_buffer = calloc(c->send_buffer_len, sizeof(uint8_t));

        snprintf((char*)c->send_buffer, c->send_buffer_len, "PUT / HTTP/1.1\r\nHost: n12-cx4:79\r\nAccept: */*\r\nContent-length: %d\r\n\r\n", c->handoff_out_ctx->client->proto_buf_len);
        memcpy(&c->send_buffer[header_len], c->handoff_out_ctx->client->proto_buf, c->handoff_out_ctx->client->proto_buf_len);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, " protobuf %s", &c->send_buffer[header_len]+sizeof(uint32_t));

        c->recv_buffer_len = 8192 * sizeof(uint8_t);
        c->read->available = c->recv_buffer_len;
        c->recv_buffer = malloc(c->read->available);

        rc = ngx_add_event(ev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT);
        assert(rc == 0);
    }
}

ngx_int_t ngx_http_handoff_out_handler(ngx_http_request_t *r) {
    ngx_int_t rc;
    ngx_connection_t *upstream_conn;
    struct handoff_out *handoff_out_ctx = r->connection->handoff_out_ctx;
    struct handoff_in  *handoff_in_ctx  = r->connection->handoff_in_ctx;
    ngx_http_handoff_main_conf_t *my_conf = ngx_http_get_module_main_conf(r, ngx_http_handoff_module);

    // fresh connection - init handoff
    if (handoff_out_ctx == NULL) {
        struct handoff_out *handoff_out_ctx = calloc(1, sizeof(struct handoff_out));
        handoff_out_ctx->ngx_conf = my_conf;
        struct http_client *client = create_http_client(0, r->connection->fd);

        struct sockaddr_in* addr = (struct sockaddr_in*)r->connection->sockaddr;
        rc = get_mac_address(my_conf->ifname, *addr, client->client_mac);
        assert(rc == 0);

        client->client_addr = addr->sin_addr.s_addr;
        client->client_port = addr->sin_port;
        strncpy(client->uri_str, (char*)r->uri.data, r->uri.len);
        client->uri_str[r->uri.len] = '\0';
        client->uri_str_len = r->uri.len;

        handoff_out_ctx->client = client;

        if (handoff_in_ctx == NULL) {
            client->to_migrate = my_random(1, handoff_out_ctx->ngx_conf->num_peers) - 1;
            client->from_migrate = -1;
        }
        else if (handoff_in_ctx != NULL) {
            client->to_migrate = -1;
            client->from_migrate = handoff_in_ctx->client_for_originaldone->from_migrate;

        }

        handoff_out_serialize(handoff_out_ctx->client, r->connection->log);
        rc = connect_to_upstream(r, handoff_out_ctx, handoff_out_connect_handler, &upstream_conn);
        assert(rc != NGX_ERROR);
    }

    return NGX_OK;
}
