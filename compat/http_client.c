/* Minimal HTTP GET client (see esp_http_client.h), Zephyr BSD sockets. */
#include "esp_http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

struct esp_http_client {
    char host[64];
    char path[128];
    char port[6];
    int  timeout_ms;
    int  fd;
    int  status;
    char pending[256];     /* body bytes read along with the headers */
    int  pending_len;
};

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    const char *u = cfg->url;
    if (strncmp(u, "http://", 7) != 0) return NULL;
    u += 7;
    struct esp_http_client *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->fd = -1;
    h->timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : 5000;
    size_t hl = strcspn(u, ":/");
    snprintf(h->host, sizeof(h->host), "%.*s", (int)hl, u);
    u += hl;
    strcpy(h->port, "80");
    if (*u == ':') {
        u++;
        size_t pl = strcspn(u, "/");
        snprintf(h->port, sizeof(h->port), "%.*s", (int)pl, u);
        u += pl;
    }
    snprintf(h->path, sizeof(h->path), "%s", *u ? u : "/");
    return h;
}

static int send_all(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t w = zsock_send(fd, b, n, 0);
        if (w <= 0) return -1;
        b += w;
        n -= w;
    }
    return 0;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t h, int write_len)
{
    struct zsock_addrinfo hints = { .ai_family = NET_AF_INET, .ai_socktype = NET_SOCK_STREAM };
    struct zsock_addrinfo *res = NULL;
    if (zsock_getaddrinfo(h->host, h->port, &hints, &res) != 0 || !res) return ESP_FAIL;
    h->fd = zsock_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (h->fd < 0) { zsock_freeaddrinfo(res); return ESP_FAIL; }
    struct zsock_timeval tv = { .tv_sec = h->timeout_ms / 1000, .tv_usec = (h->timeout_ms % 1000) * 1000 };
    zsock_setsockopt(h->fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
    zsock_setsockopt(h->fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &tv, sizeof(tv));
    int r = zsock_connect(h->fd, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    if (r < 0) return ESP_FAIL;
    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: c6watch\r\nConnection: close\r\n\r\n",
                     h->path, h->host);
    return send_all(h->fd, req, n) == 0 ? ESP_OK : ESP_FAIL;
}

int esp_http_client_fetch_headers(esp_http_client_handle_t h)
{
    char buf[512];
    int len = 0;
    for (;;) {
        if (len >= (int)sizeof(buf) - 1) return -1;
        ssize_t r = zsock_recv(h->fd, buf + len, sizeof(buf) - 1 - len, 0);
        if (r <= 0) return -1;
        len += r;
        buf[len] = 0;
        char *end = strstr(buf, "\r\n\r\n");
        if (!end) continue;
        sscanf(buf, "HTTP/%*s %d", &h->status);
        end += 4;
        h->pending_len = len - (end - buf);
        if (h->pending_len > (int)sizeof(h->pending)) h->pending_len = sizeof(h->pending);
        memcpy(h->pending, end, h->pending_len);
        return 0;
    }
}

int esp_http_client_get_status_code(esp_http_client_handle_t h)
{
    return h->status;
}

int esp_http_client_read_response(esp_http_client_handle_t h, char *buf, int len)
{
    int n = MIN(len, h->pending_len);
    memcpy(buf, h->pending, n);
    h->pending_len = 0;
    while (n < len) {
        ssize_t r = zsock_recv(h->fd, buf + n, len - n, 0);
        if (r <= 0) break;
        n += r;
    }
    return n;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h)
{
    if (!h) return ESP_OK;
    if (h->fd >= 0) zsock_close(h->fd);
    free(h);
    return ESP_OK;
}
