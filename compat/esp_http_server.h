/* A small ESP-IDF-style HTTP server (the calls the watch uses) on BSD sockets. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"

typedef enum { HTTP_GET = 1, HTTP_POST = 3, HTTP_PUT = 4, HTTP_DELETE = 0, HTTP_HEAD = 2 } httpd_method_t;

typedef enum {
    HTTPD_400_BAD_REQUEST = 400,
    HTTPD_404_NOT_FOUND = 404,
    HTTPD_405_METHOD_NOT_ALLOWED = 405,
    HTTPD_408_REQ_TIMEOUT = 408,
    HTTPD_411_LENGTH_REQUIRED = 411,
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
} httpd_err_code_t;

#define HTTPD_SOCK_ERR_FAIL     -1
#define HTTPD_SOCK_ERR_INVALID  -2
#define HTTPD_SOCK_ERR_TIMEOUT  -3
#define HTTPD_RESP_USE_STRLEN   -1

typedef struct httpd_server *httpd_handle_t;

typedef struct httpd_req {
    httpd_handle_t handle;
    int            method;
    char           uri[128];
    size_t         content_len;
    void          *user_ctx;
    /* private */
    int            fd;
    size_t         body_left;
    bool           headers_sent;
    bool           chunked;
    int            timeouts;      /* consecutive receive timeouts */
    char           status[32];
    char           type[48];
} httpd_req_t;

typedef bool (*httpd_uri_match_func_t)(const char *tmpl, const char *uri, size_t len);

typedef struct httpd_uri {
    const char     *uri;
    httpd_method_t  method;
    esp_err_t     (*handler)(httpd_req_t *r);
    void           *user_ctx;
} httpd_uri_t;

typedef struct {
    uint16_t server_port;
    uint16_t max_uri_handlers;
    uint32_t stack_size;
    int      task_priority;
    int      recv_wait_timeout;   /* s */
    int      send_wait_timeout;   /* s */
    bool     lru_purge_enable;
    uint16_t max_open_sockets;
    httpd_uri_match_func_t uri_match_fn;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() { .server_port = 80, .max_uri_handlers = 12, .stack_size = 6144, \
                                 .task_priority = 5, .recv_wait_timeout = 5, .send_wait_timeout = 5 }

esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *cfg);
esp_err_t httpd_stop(httpd_handle_t h);
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *uri);
bool      httpd_uri_match_wildcard(const char *tmpl, const char *uri, size_t len);

int       httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t len);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t code, const char *msg);
static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    return httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN);
}
static inline esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *s)
{
    return httpd_resp_send_chunk(r, s, s ? HTTPD_RESP_USE_STRLEN : 0);
}
#define httpd_resp_send_404(r) httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, NULL)
#define httpd_resp_send_500(r) httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, NULL)
