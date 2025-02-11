#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <sys/ioctl.h>
#include <net/if.h>

#include <net/if_arp.h>

#include "util.h"
#include "handoff.h"
#include "http_client.h"

#include "connect.h"
#include "handoff_in.h"

static char *ngx_http_handoff_set_target(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_handoff_set_ifname(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static char *ngx_http_handoff_out(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_handoff_in(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_http_handoff_out_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_handoff_in_handler(ngx_http_request_t *r);

static void *ngx_http_handoff_create_main_conf(ngx_conf_t *cf);

extern uint8_t *eight_MB;

static int my_random(int min, int max){
   return min + rand() / (RAND_MAX / (max - min + 1) + 1);
}

static ngx_command_t ngx_http_handoff_commands[] = {
    { ngx_string("handoff_out"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_handoff_out,
      0,
      0,
      NULL },

    { ngx_string("handoff_in"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_handoff_in,
      0,
      0,
      NULL },

    { ngx_string("handoff_ifname"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_handoff_set_ifname,
      0,
      0,
      NULL },


    { ngx_string("handoff_target"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_handoff_set_target,
      0,
      0,
      NULL },

      ngx_null_command
};

static ngx_http_module_t ngx_http_handoff_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    ngx_http_handoff_create_main_conf, /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL,   /* merge location configuration */
};

ngx_module_t ngx_http_handoff_module = {
    NGX_MODULE_V1,
    &ngx_http_handoff_module_ctx,           /* module context */
    ngx_http_handoff_commands,              /* module directives */
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
    clcf->handler = ngx_http_handoff_out_handler;

    return NGX_CONF_OK;
}

static char *ngx_http_handoff_in(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_core_loc_conf_t *clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_handoff_in_handler;

    eight_MB = malloc(sizeof(uint8_t) * 1024 * 1024 * 8);
    memset(eight_MB, 1, sizeof(uint8_t) * 1024 * 1024 * 8);

    return NGX_CONF_OK;
}

static char *ngx_http_handoff_set_ifname(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_handoff_main_conf_t *my_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_handoff_module);

    ngx_str_t *value = cf->args->elts;
    if (cf->args->nelts != 2) {
        return NGX_CONF_ERROR;
    }

    strncpy(my_conf->ifname, (char*)value[1].data, value[1].len);
    my_conf->ifname[value[1].len] = 0;

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

    return NGX_CONF_OK;
}

static char *ngx_http_handoff_set_target(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_handoff_main_conf_t *my_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_handoff_module);

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

static ngx_int_t ngx_http_handoff_out_handler(ngx_http_request_t *r) {
    ngx_int_t rc;
    ngx_connection_t *upstream_conn;

    // fresh connection - init handoff
    if (r->connection->handoff_in_ctx == NULL) {
        ngx_http_handoff_main_conf_t *my_conf = ngx_http_get_module_main_conf(r, ngx_http_handoff_module);
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

        rc = connect_to_upstream(r, handoff_out_ctx, handoff_out_connect_handler, &upstream_conn);
        assert(rc != NGX_ERROR);

        return NGX_OK;
    }
    return NGX_OK;
}

static ngx_int_t ngx_http_handoff_in_handler(ngx_http_request_t *r) {
    ngx_int_t rc;

    if (r->connection->handoff_in_ctx == NULL) {
        r->connection->handoff_in_ctx = calloc(1, sizeof(struct handoff_in));
        r->connection->handoff_in_ctx->ngx_conf = ngx_http_get_module_main_conf(r, ngx_http_handoff_module);
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "receve incoming handoff");

        r->request_body_in_single_buf = 1;
        r->keepalive = 1;
        rc = ngx_http_read_client_request_body(r, ngx_http_handoff_in_init);
        if (rc != NGX_OK) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return NGX_ERROR;
        }
        return NGX_OK;
    }
    else if (r->connection->handoff_in_ctx != NULL && r->connection->handoff_in_ctx->wait_for_originaldone) {
        ngx_connection_t *restored_conn = r->connection->handoff_in_ctx->restored_conn;
        ngx_blocking(restored_conn->fd);

        r->connection->handoff_in_ctx->wait_for_originaldone = false;
        //r->connection->handoff_in_ctx->restored_conn->handoff_in_ctx->wait_for_originaldone = false;
if (r->connection->handoff_in_ctx->client_for_originaldone == NULL) {
ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "client struct is empty!!!!!!!!!!!!!!!!!!!!l ");
exit(1);
}
ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "sending first OK to client uri: %s", r->connection->handoff_in_ctx->client_for_originaldone->uri_str);

        int payload_size = 0;
        if (strlen(r->connection->handoff_in_ctx->client_for_originaldone->uri_str) > 1)
            payload_size = atoi(r->connection->handoff_in_ctx->client_for_originaldone->uri_str + sizeof(char));
        size_t total_header_len = snprintf(NULL, 0, "HTTP/1.1 200 OK\r\nServer: nginx/1.27.3\r\nDate: Fri, 31 Jan 2025 01:26:51 GMT\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n", payload_size);
        uint8_t *buffer = malloc(total_header_len * sizeof(char) + 1 + payload_size + 1);
        memset(buffer, 1, total_header_len * sizeof(char) + 1 + payload_size + 1);
        snprintf((char*)buffer, total_header_len + 1, "HTTP/1.1 200 OK\r\nServer: nginx/1.27.3\r\nDate: Fri, 31 Jan 2025 01:26:51 GMT\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n", payload_size);

        restored_conn->send(restored_conn, buffer, total_header_len*sizeof(char) + payload_size);
        free(buffer);
        // handle unblocking and reply to client ehre
        ngx_nonblocking(restored_conn->fd);

        //r->keepalive = 1;
        rc = ngx_http_discard_request_body(r);
        ngx_http_finalize_request(r, NGX_OK);
        ngx_close_connection(r->connection);

        return NGX_OK;
    }

    return xo_handle_http_request(r);
}

static void *ngx_http_handoff_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_handoff_main_conf_t  *my_conf;

    my_conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_handoff_main_conf_t));
    if (my_conf == NULL) {
        return NULL;
    }
    my_conf->num_peers = 0;

    return my_conf;
}
