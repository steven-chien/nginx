#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <sys/ioctl.h>
#include <net/if.h>

#include <net/if_arp.h>

#include "util.h"
#include "handoff.h"
#include "http_client.h"

static char *ngx_http_handoff_out(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_handoff_out_set_target(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_handoff_out_handler(ngx_http_request_t *r);

static ngx_command_t ngx_http_handoff_out_commands[] = {
    { ngx_string("handoff_out"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_handoff_out,
      0,
      0,
      NULL },

    { ngx_string("handoff_target"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_handoff_out_set_target,
      0,
      0,
      NULL },

      ngx_null_command
};

static int my_random(int min, int max){
   return min + rand() / (RAND_MAX / (max - min + 1) + 1);
}

static int get_mac_address(const char *ifname, struct sockaddr_in addr, uint8_t *mac) {
    struct arpreq arp_req;
    int sock_fd;
    memset(&arp_req, 0, sizeof(struct arpreq));
    struct sockaddr_in *sin = (struct sockaddr_in *)&arp_req.arp_pa;
    sin->sin_family = AF_INET;
    sin->sin_addr = addr.sin_addr;

    if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket failed");
        return -1;
    }

    strncpy(arp_req.arp_dev, ifname, IFNAMSIZ-1);
    if (ioctl(sock_fd, SIOCGARP, &arp_req) == -1) {
        close(sock_fd);
        return -1;
    }

    memcpy(mac, arp_req.arp_ha.sa_data, sizeof(uint8_t) * 6);

    close(sock_fd);
    return 0;
}

static void *
ngx_http_handoff_out_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_handoff_out_loc_conf_t  *my_conf;

    my_conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_handoff_out_loc_conf_t));
    if (my_conf == NULL) {
        return NULL;
    }
    my_conf->num_peers = 0;

    return my_conf;
}

static ngx_http_module_t ngx_http_handoff_out_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_handoff_out_create_loc_conf,  /* create location configuration */
    NULL,   /* merge location configuration */
};

ngx_module_t ngx_http_handoff_out_module = {
    NGX_MODULE_V1,
    &ngx_http_handoff_out_module_ctx,           /* module context */
    ngx_http_handoff_out_commands,              /* module directives */
    NGX_HTTP_MODULE,                      /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static char *ngx_http_handoff_out(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_core_loc_conf_t *clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    ngx_http_handoff_out_loc_conf_t *my_conf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_handoff_out_module);

    ngx_str_t *value = cf->args->elts;
    if (cf->args->nelts != 2) {
        return NGX_CONF_ERROR;
    }

    strncpy(my_conf->ifname, (char*)value[1].data, value[1].len);
    my_conf->ifname[value[1].len] = 0;

#include "util.h"
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, my_conf->ifname , IFNAMSIZ);

    if (ioctl(fd, SIOCGIFADDR, &ifr) == -1 ) {
        close(fd);
        exit(EXIT_FAILURE);
    }

    memcpy(&my_conf->my_sockaddr, (struct sockaddr_in *)&ifr.ifr_addr, sizeof(struct sockaddr_in));

    // Perform the IOCTL operation to fetch the hardware address
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1) {
        close(fd);
        exit(EXIT_FAILURE);
    }

    memcpy(my_conf->my_mac, ifr.ifr_hwaddr.sa_data, 6);
    close(fd);

    // init lib forward
    int err = init_forward(my_conf->ifname, "ingress", "1:");
    assert( err >= 0 );

    clcf->handler = ngx_http_handoff_out_handler;

    return NGX_CONF_OK;
}

static char *ngx_http_handoff_out_set_target(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_handoff_out_loc_conf_t *my_conf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_handoff_out_module);

    ngx_str_t *value = cf->args->elts;
    if (cf->args->nelts != 3) {
        return NGX_CONF_ERROR;
    }

    if (my_conf->num_peers > MAX_PEERS) {
        return NGX_CONF_ERROR;
    }

    // backend
    memset(&my_conf->peer_sockaddr[my_conf->num_peers], 0, sizeof(my_conf->peer_sockaddr[my_conf->num_peers]));
    inet_pton(AF_INET, (char*)value[1].data, &my_conf->peer_sockaddr[my_conf->num_peers].sin_addr);
    my_conf->peer_sockaddr[my_conf->num_peers].sin_port = htons(ngx_atoi(value[2].data, value[2].len));
    my_conf->peer_sockaddr[my_conf->num_peers].sin_family = AF_INET;
    printf("xo target %d %s:%ld\n", my_conf->num_peers, (char*)value[1].data, ngx_atoi(value[2].data, value[2].len));
    my_conf->num_peers++;

    return NGX_CONF_OK;
}

static
void handoff_out_read_handler(ngx_event_t *ev) {
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
ngx_blocking(c->fd);

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0, "upstream sock read event fd=%d received=%d: %s", c->fd, rc, c->recv_buffer);
        rc = ngx_del_event(ev, NGX_READ_EVENT, NGX_CLEAR_EVENT); 
        assert(rc == 0);

        // apply redirection
        struct handoff_out *handoff_out_ctx = c->handoff_out_ctx;
        ngx_http_handoff_out_loc_conf_t *my_conf = handoff_out_ctx->ngx_conf;

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
                                    migration_info->peer_addr, my_conf->my_mac, my_conf->peer_sockaddr[handoff_out_ctx->peer_to_connect].sin_addr.s_addr, fake_server_mac,
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

static
void handoff_out_write_handler(ngx_event_t *ev) {
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

static
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

static ngx_int_t connect_to_upstream(ngx_http_request_t *r, struct handoff_out *handoff_out_ctx, ngx_connection_t **conn) {
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

    rev->handler = handoff_out_connect_handler;
    wev->handler = handoff_out_connect_handler;
    upstream_conn->read = rev;
    upstream_conn->write = wev;

    rev->log = upstream_conn->log;
    wev->log = upstream_conn->log;

    rev->data = upstream_conn;
    wev->data = upstream_conn;

    struct sockaddr_in sockaddr;
    memcpy(&sockaddr, &handoff_out_ctx->ngx_conf->peer_sockaddr[handoff_out_ctx->peer_to_connect], sizeof(struct sockaddr_in));

    if (ngx_add_conn) {
        rc = ngx_add_conn(upstream_conn);
        assert(rc != NGX_ERROR);
    }
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, r->connection->log, 0,
                   "connect to upstream peer %d, fd:%d #%uA", upstream_conn->handoff_out_ctx->peer_to_connect, upstream_conn->number);

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

static ngx_int_t ngx_http_handoff_out_handler(ngx_http_request_t *r) {
    ngx_int_t rc;
    ngx_connection_t *upstream_conn;

    // fresh connection - init handoff
    if (r->connection->handoff_in_ctx == NULL) {
        ngx_http_handoff_out_loc_conf_t *my_conf = ngx_http_get_module_loc_conf(r, ngx_http_handoff_out_module);
        struct handoff_out *handoff_out_ctx = calloc(1, sizeof(struct handoff_out));

        handoff_out_ctx->ngx_conf = my_conf;
        handoff_out_ctx->peer_to_connect = my_random(1, handoff_out_ctx->ngx_conf->num_peers) - 1;

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
        handoff_out_serialize(handoff_out_ctx->client, r->connection->log);

        rc = connect_to_upstream(r, handoff_out_ctx, &upstream_conn);
        assert(rc != NGX_ERROR);

        return NGX_OK;
    }
    return NGX_OK;
}
