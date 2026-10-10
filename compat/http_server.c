/* Small HTTP/1.1 server for the ESP-IDF httpd API (see esp_http_server.h),
 * on Zephyr BSD sockets.  One connection at a time, "Connection: close". */
#include "esp_http_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "httpd";

#define MAX_HANDLERS 16
#define HDR_MAX      1024

struct httpd_server {
    httpd_config_t cfg;
    httpd_uri_t    uris[MAX_HANDLERS];
    int            n_uris;
    int            listen_fd;
    volatile bool  stop;
    TaskHandle_t   task;
    struct k_sem   done;
    /* body bytes read along with the headers (points into the header buffer) */
    const char    *extra;
    int            extra_len, extra_pos;
};

/* ── Matching ─────────────────────────────────────────────────────────── */
bool httpd_uri_match_wildcard(const char *tmpl, const char *uri, size_t len)
{
    size_t tl = strlen(tmpl);
    if (tl && tmpl[tl - 1] == '*') {
        tl--;
        if (tl && tmpl[tl - 1] == '/' && len == tl - 1 && !strncmp(tmpl, uri, len)) return true;
        return len >= tl && !strncmp(tmpl, uri, tl);
    }
    if (tl && tmpl[tl - 1] == '?') {
        tl--;
        if (len == tl - 1 && !strncmp(tmpl, uri, len)) return true;
    }
    return len == tl && !strncmp(tmpl, uri, len);
}

/* ── Socket helpers ───────────────────────────────────────────────────── */
static int send_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = zsock_send(fd, buf, len, 0);
        if (n <= 0) return -1;
        buf += n;
        len -= n;
    }
    return 0;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    struct httpd_server *s = r->handle;
    if (r->body_left == 0) return 0;
    if (len > r->body_left) len = r->body_left;
    if (s->extra_pos < s->extra_len) {
        size_t n = s->extra_len - s->extra_pos;
        if (n > len) n = len;
        memcpy(buf, s->extra + s->extra_pos, n);
        s->extra_pos += n;
        r->body_left -= n;
        return n;
    }
    ssize_t n = zsock_recv(r->fd, buf, len, 0);
    if (n < 0 && errno == EAGAIN) {
        /* Callers retry timeouts; give up on a client that went away */
        return ++r->timeouts < 6 ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
    }
    if (n <= 0) return HTTPD_SOCK_ERR_FAIL;
    r->timeouts = 0;
    r->body_left -= n;
    return n;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len)
{
    const char *q = strchr(r->uri, '?');
    if (!q) return ESP_ERR_NOT_FOUND;
    if (strlen(q + 1) >= len) return ESP_ERR_INVALID_SIZE;
    strcpy(buf, q + 1);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t len)
{
    size_t kl = strlen(key);
    const char *p = qry;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t n = strcspn(p, "&");
            if (n >= len) n = len - 1;
            memcpy(val, p, n);
            val[n] = 0;
            return ESP_OK;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    snprintf(r->status, sizeof(r->status), "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    snprintf(r->type, sizeof(r->type), "%s", type);
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    return ESP_OK;
}

static int send_headers(httpd_req_t *r, long len)
{
    char h[200];
    int n;
    if (len >= 0) {
        n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                     "Connection: close\r\n\r\n", r->status, r->type, len);
    } else {
        n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
                     "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n", r->status, r->type);
        r->chunked = true;
    }
    r->headers_sent = true;
    return send_all(r->fd, h, n);
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len)
{
    if (len == HTTPD_RESP_USE_STRLEN) len = buf ? strlen(buf) : 0;
    if (r->headers_sent) return ESP_FAIL;
    if (send_headers(r, len) || (len && send_all(r->fd, buf, len))) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len)
{
    if (len == HTTPD_RESP_USE_STRLEN) len = buf ? strlen(buf) : 0;
    if (!r->headers_sent && send_headers(r, -1)) return ESP_FAIL;
    char h[16];
    int n = snprintf(h, sizeof(h), "%zx\r\n", (size_t)len);
    if (send_all(r->fd, h, n)) return ESP_FAIL;
    if (len && send_all(r->fd, buf, len)) return ESP_FAIL;
    return send_all(r->fd, "\r\n", 2) ? ESP_FAIL : ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t code, const char *msg)
{
    const char *text = code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
                       code == 405 ? "Method Not Allowed" : code == 408 ? "Request Timeout" :
                       code == 411 ? "Length Required" : "Internal Server Error";
    snprintf(r->status, sizeof(r->status), "%d %s", code, text);
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_send(r, msg ? msg : text, HTTPD_RESP_USE_STRLEN);
}

/* ── Request parsing ──────────────────────────────────────────────────── */
static int method_of(const char *m)
{
    if (!strcmp(m, "GET"))    return HTTP_GET;
    if (!strcmp(m, "POST"))   return HTTP_POST;
    if (!strcmp(m, "PUT"))    return HTTP_PUT;
    if (!strcmp(m, "DELETE")) return HTTP_DELETE;
    if (!strcmp(m, "HEAD"))   return HTTP_HEAD;
    return -1;
}

static void serve(struct httpd_server *s, int fd)
{
    char *hdr = malloc(HDR_MAX + 1);
    if (!hdr) return;
    int got = 0;
    char *end = NULL;
    while (got < HDR_MAX) {
        ssize_t n = zsock_recv(fd, hdr + got, HDR_MAX - got, 0);
        if (n <= 0) goto out;
        got += n;
        hdr[got] = 0;
        if ((end = strstr(hdr, "\r\n\r\n"))) break;
    }
    if (!end) goto out;

    httpd_req_t *r = calloc(1, sizeof(*r));
    if (!r) goto out;
    r->handle = s;
    r->fd = fd;
    strcpy(r->status, "200 OK");
    strcpy(r->type, "text/html");

    char method[8] = "";
    sscanf(hdr, "%7s %127s", method, r->uri);
    r->method = method_of(method);

    for (char *line = strstr(hdr, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n")) {
        if (!strncasecmp(line + 2, "Content-Length:", 15)) {
            r->content_len = strtoul(line + 17, NULL, 10);
        }
    }
    r->body_left = r->content_len;

    char *body = end + 4;
    s->extra = body;
    s->extra_len = got - (body - hdr);
    s->extra_pos = 0;

    size_t path_len = strcspn(r->uri, "?");
    httpd_uri_match_func_t match = s->cfg.uri_match_fn;
    const httpd_uri_t *h = NULL;
    bool path_found = false;
    for (int i = 0; i < s->n_uris; i++) {
        bool m = match ? match(s->uris[i].uri, r->uri, path_len)
                       : (strlen(s->uris[i].uri) == path_len &&
                          !strncmp(s->uris[i].uri, r->uri, path_len));
        if (!m) continue;
        path_found = true;
        if ((int)s->uris[i].method == r->method) { h = &s->uris[i]; break; }
    }

    if (h) {
        r->user_ctx = h->user_ctx;
        if (h->handler(r) != ESP_OK && !r->headers_sent) {
            httpd_resp_send_500(r);
        }
    } else {
        httpd_resp_send_err(r, path_found ? HTTPD_405_METHOD_NOT_ALLOWED : HTTPD_404_NOT_FOUND,
                            NULL);
    }
    free(r);
out:
    free(hdr);
}

static void server_task(void *arg)
{
    struct httpd_server *s = arg;
    while (!s->stop) {
        struct zsock_pollfd pfd = { .fd = s->listen_fd, .events = ZSOCK_POLLIN };
        if (zsock_poll(&pfd, 1, 500) <= 0) continue;
        int fd = zsock_accept(s->listen_fd, NULL, NULL);
        if (fd < 0) continue;
        struct zsock_timeval tv = { .tv_sec = s->cfg.recv_wait_timeout ? s->cfg.recv_wait_timeout : 5 };
        zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
        tv.tv_sec = s->cfg.send_wait_timeout ? s->cfg.send_wait_timeout : 5;
        zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &tv, sizeof(tv));
        serve(s, fd);
        zsock_shutdown(fd, ZSOCK_SHUT_WR);
        zsock_close(fd);
    }
    zsock_close(s->listen_fd);
    k_sem_give(&s->done);
    vTaskDelete(NULL);
}

esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *cfg)
{
    struct httpd_server *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->cfg = *cfg;
    k_sem_init(&s->done, 0, 1);

    s->listen_fd = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
    if (s->listen_fd < 0) { free(s); return ESP_FAIL; }
    int one = 1;
    zsock_setsockopt(s->listen_fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one));
    struct net_sockaddr_in addr = {
        .sin_family = NET_AF_INET,
        .sin_port   = net_htons(cfg->server_port ? cfg->server_port : 80),
    };
    if (zsock_bind(s->listen_fd, (struct net_sockaddr *)&addr, sizeof(addr)) < 0 ||
        zsock_listen(s->listen_fd, 2) < 0) {
        ESP_LOGE(TAG, "bind/listen failed (%d)", errno);
        zsock_close(s->listen_fd);
        free(s);
        return ESP_FAIL;
    }
    if (xTaskCreate(server_task, "httpd", cfg->stack_size ? cfg->stack_size : 6144, s,
                    cfg->task_priority ? cfg->task_priority : 5, &s->task) != pdPASS) {
        zsock_close(s->listen_fd);
        free(s);
        return ESP_ERR_NO_MEM;
    }
    *out = s;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    h->stop = true;
    k_sem_take(&h->done, K_SECONDS(30));
    k_msleep(50);              /* let the task exit */
    free(h);
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *uri)
{
    if (!h || h->n_uris >= MAX_HANDLERS) return ESP_ERR_NO_MEM;
    h->uris[h->n_uris++] = *uri;
    return ESP_OK;
}
