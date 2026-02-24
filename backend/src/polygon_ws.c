/*
 * polygon_ws.c  –  WebSocket client for Polygon.io stocks feed.
 *
 * Protocol:
 *  1. TLS connect to delayed.polygon.io:443
 *  2. HTTP/1.1 Upgrade handshake (RFC 6455)
 *  3. Receive auth-required event, send auth action
 *  4. Subscribe/unsubscribe with Q.* (quotes) messages
 *  5. Parse incoming JSON frames and update MarketState
 *
 * Dependencies: OpenSSL (libssl, libcrypto), cJSON
 */

#include "polygon_ws.h"
#include "market_data.h"
#include "ipc_server.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define sleep(s) Sleep((s)*1000)
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define sock_close(s) closesocket(s)
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  typedef int sock_t;
  #define SOCK_INVALID -1
  #define sock_close(s) close(s)
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

/* ── WebSocket constants ─────────────────────────────────────────── */
#define WS_HOST "delayed.polygon.io"
#define WS_PORT "443"
#define WS_PATH "/stocks"

#define WS_FRAME_TEXT   0x81
#define WS_FRAME_CLOSE  0x88
#define WS_FRAME_PING   0x89
#define WS_FRAME_PONG   0x8A

#define RECV_BUF 65536

/* ── Thread state ─────────────────────────────────────────────────── */
static pthread_t      ws_thread;
static volatile int   ws_running  = 0;
static char           ws_api_key[256];
static SSL_CTX       *ssl_ctx     = NULL;
static SSL           *ssl_conn    = NULL;
static sock_t         ws_sock     = SOCK_INVALID;

/* pending subscribe requests (set from any thread, consumed by WS thread) */
static pthread_mutex_t sub_mutex = PTHREAD_MUTEX_INITIALIZER;
static char  sub_queue[MAX_SYMBOLS][MAX_SYMBOL_LEN];
static int   sub_flags[MAX_SYMBOLS];   /* 1=subscribe, 0=unsubscribe */
static int   sub_queue_len = 0;

/* ── Low-level TLS helpers ───────────────────────────────────────── */

static sock_t tcp_connect(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res, *rp;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        fprintf(stderr, "[WS] getaddrinfo failed\n");
        return SOCK_INVALID;
    }

    sock_t s = SOCK_INVALID;
    for (rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == SOCK_INVALID) continue;
        if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        sock_close(s);
        s = SOCK_INVALID;
    }
    freeaddrinfo(res);
    return s;
}

/* ── WebSocket handshake ─────────────────────────────────────────── */

static void base64_encode(const unsigned char *in, int len, char *out) {
    static const char enc[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, j = 0;
    while (i < len) {
        unsigned int a = in[i++];
        unsigned int b = (i < len) ? in[i++] : 0;
        unsigned int c = (i < len) ? in[i++] : 0;
        out[j++] = enc[(a >> 2) & 0x3F];
        out[j++] = enc[((a & 3) << 4) | ((b >> 4) & 0xF)];
        out[j++] = (i > len + 1) ? '=' : enc[((b & 0xF) << 2) | ((c >> 6) & 3)];
        out[j++] = (i > len)     ? '=' : enc[c & 0x3F];
    }
    out[j] = '\0';
}

static int ws_handshake(void) {
    unsigned char nonce[16];
    RAND_bytes(nonce, 16);
    char key_b64[32];
    base64_encode(nonce, 16, key_b64);

    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        WS_PATH, WS_HOST, key_b64);

    if (SSL_write(ssl_conn, req, (int)strlen(req)) <= 0) {
        fprintf(stderr, "[WS] Handshake write failed\n");
        return -1;
    }

    char resp[2048] = {0};
    int  total      = 0;
    while (total < (int)sizeof(resp) - 1) {
        int n = SSL_read(ssl_conn, resp + total, sizeof(resp) - 1 - total);
        if (n <= 0) break;
        total += n;
        if (strstr(resp, "\r\n\r\n")) break;
    }

    if (!strstr(resp, "101 Switching Protocols")) {
        fprintf(stderr, "[WS] Unexpected HTTP response:\n%s\n", resp);
        return -1;
    }
    return 0;
}

/* ── WebSocket frame send (client must mask payload per RFC 6455) ─── */

static int ws_send_text(const char *msg) {
    size_t plen   = strlen(msg);
    uint8_t mask[4];
    RAND_bytes(mask, 4);

    uint8_t header[10];
    int     hlen = 2;
    header[0] = 0x81; /* FIN + text opcode */
    if (plen < 126) {
        header[1] = 0x80 | (uint8_t)plen;
    } else if (plen < 65536) {
        header[1] = 0x80 | 126;
        header[2] = (plen >> 8) & 0xFF;
        header[3] = plen & 0xFF;
        hlen = 4;
    } else {
        fprintf(stderr, "[WS] Message too large\n");
        return -1;
    }
    memcpy(header + hlen, mask, 4);
    hlen += 4;

    uint8_t *payload = malloc(plen);
    if (!payload) return -1;
    for (size_t i = 0; i < plen; i++)
        payload[i] = ((uint8_t)msg[i]) ^ mask[i % 4];

    SSL_write(ssl_conn, header, hlen);
    SSL_write(ssl_conn, payload, (int)plen);
    free(payload);
    return 0;
}

/* Send pong in response to ping */
static void ws_send_pong(const uint8_t *payload, size_t plen) {
    uint8_t hdr[6];
    hdr[0] = WS_FRAME_PONG;
    hdr[1] = 0x80 | (uint8_t)(plen < 126 ? plen : 0);
    uint8_t mask[4] = {0};
    memcpy(hdr + 2, mask, 4);
    SSL_write(ssl_conn, hdr, 6);
    if (plen > 0) SSL_write(ssl_conn, payload, (int)plen);
}

/* ── WebSocket frame receive ─────────────────────────────────────── */

/* Returns number of bytes of TEXT payload written to buf (NUL-terminated).
   Returns 0 for control frames, -1 on error/close. */
static int ws_recv_frame(char *buf, int bufsz) {
    uint8_t header[2];
    if (SSL_read(ssl_conn, header, 2) != 2) return -1;

    int  opcode = header[0] & 0x0F;
    int  masked  = (header[1] >> 7) & 1;
    int64_t plen = header[1] & 0x7F;

    if (plen == 126) {
        uint8_t ext[2];
        if (SSL_read(ssl_conn, ext, 2) != 2) return -1;
        plen = ((int64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (SSL_read(ssl_conn, ext, 8) != 8) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (SSL_read(ssl_conn, mask, 4) != 4) return -1;
    }

    if (opcode == 0x8) return -1;  /* CLOSE */
    if (opcode == 0x9) {           /* PING  */
        uint8_t ping_payload[125] = {0};
        int n = (plen > 0) ? SSL_read(ssl_conn, ping_payload, (int)plen) : 0;
        ws_send_pong(ping_payload, n > 0 ? n : 0);
        return 0;
    }
    if (opcode != 0x1 && opcode != 0x0) {
        /* skip unknown frames */
        while (plen > 0) {
            char tmp[256];
            int n = SSL_read(ssl_conn, tmp,
                             (int)(plen > 256 ? 256 : plen));
            if (n <= 0) return -1;
            plen -= n;
        }
        return 0;
    }

    /* Read text payload */
    int64_t read_total = 0;
    while (read_total < plen && read_total < bufsz - 1) {
        int n = SSL_read(ssl_conn, buf + read_total,
                         (int)(plen - read_total < bufsz - 1 - read_total
                               ? plen - read_total
                               : bufsz - 1 - read_total));
        if (n <= 0) return -1;
        read_total += n;
    }
    buf[read_total] = '\0';

    if (masked)
        for (int64_t i = 0; i < read_total; i++)
            buf[i] ^= mask[i % 4];

    return (int)read_total;
}

/* ── JSON helpers ────────────────────────────────────────────────── */

/* Broadcast a JSON string to all IPC clients */
static void broadcast(const char *json) {
    ipc_broadcast(json);
}

/* Parse a Polygon.io quote event (ev=Q) */
static void parse_quote(cJSON *ev) {
    cJSON *sym = cJSON_GetObjectItemCaseSensitive(ev, "sym");
    cJSON *bp  = cJSON_GetObjectItemCaseSensitive(ev, "bp");  /* bid price */
    cJSON *ap  = cJSON_GetObjectItemCaseSensitive(ev, "ap");  /* ask price */
    cJSON *lp  = cJSON_GetObjectItemCaseSensitive(ev, "lp");  /* last price */
    cJSON *av  = cJSON_GetObjectItemCaseSensitive(ev, "av");  /* accum vol */
    cJSON *t   = cJSON_GetObjectItemCaseSensitive(ev, "t");   /* timestamp */

    if (!sym || !cJSON_IsString(sym)) return;

    double price  = cJSON_IsNumber(lp) ? lp->valuedouble :
                   (cJSON_IsNumber(bp) ? bp->valuedouble : 0.0);
    double bid    = cJSON_IsNumber(bp) ? bp->valuedouble : 0.0;
    double ask    = cJSON_IsNumber(ap) ? ap->valuedouble : 0.0;
    int64_t vol   = cJSON_IsNumber(av) ? (int64_t)av->valuedouble : 0;
    int64_t ts    = cJSON_IsNumber(t)  ? (int64_t)t->valuedouble  : 0;

    market_update_quote(sym->valuestring, price, bid, ask, vol, ts);

    /* forward to Java clients */
    char msg[512];
    snprintf(msg, sizeof(msg),
        "{\"type\":\"quote\",\"symbol\":\"%s\","
        "\"price\":%.4f,\"bid\":%.4f,\"ask\":%.4f,"
        "\"volume\":%lld,\"ts\":%lld}\n",
        sym->valuestring, price, bid, ask,
        (long long)vol, (long long)ts);
    broadcast(msg);

    /* check alerts */
    pthread_mutex_lock(&g_state.lock);
    for (int i = 0; i < g_state.alert_count; i++) {
        AlertRecord *a = &g_state.alerts[i];
        if (!a->active) continue;
        if (strcmp(a->symbol, sym->valuestring) != 0) continue;
        int fired = (a->type == ALERT_ABOVE && price >= a->trigger_price) ||
                    (a->type == ALERT_BELOW && price <= a->trigger_price);
        if (fired) {
            char alert_msg[512];
            snprintf(alert_msg, sizeof(alert_msg),
                "{\"type\":\"alert\",\"id\":%d,\"symbol\":\"%s\","
                "\"condition\":\"%s\",\"trigger\":%.4f,\"price\":%.4f}\n",
                a->id, a->symbol,
                a->type == ALERT_ABOVE ? "above" : "below",
                a->trigger_price, price);
            pthread_mutex_unlock(&g_state.lock);
            broadcast(alert_msg);
            pthread_mutex_lock(&g_state.lock);
            a->active = 0;  /* one-shot alert */
        }
    }
    pthread_mutex_unlock(&g_state.lock);
}

/* Process one complete Polygon.io message (may be array of events) */
static void process_message(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    /* Polygon sends arrays of event objects */
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        cJSON *ev_type = cJSON_GetObjectItemCaseSensitive(item, "ev");
        if (!ev_type || !cJSON_IsString(ev_type)) continue;

        if (strcmp(ev_type->valuestring, "Q") == 0 ||
            strcmp(ev_type->valuestring, "T") == 0) {
            parse_quote(item);
        } else if (strcmp(ev_type->valuestring, "status") == 0) {
            cJSON *status = cJSON_GetObjectItemCaseSensitive(item, "status");
            cJSON *msg    = cJSON_GetObjectItemCaseSensitive(item, "message");
            if (status && cJSON_IsString(status))
                printf("[WS] Status: %s  %s\n",
                       status->valuestring,
                       msg && cJSON_IsString(msg) ? msg->valuestring : "");

            /* After auth success, flush the subscribe queue */
            if (status && strcmp(status->valuestring, "auth_success") == 0) {
                pthread_mutex_lock(&sub_mutex);
                for (int i = 0; i < sub_queue_len; i++) {
                    char sub_msg[256];
                    snprintf(sub_msg, sizeof(sub_msg),
                        "{\"action\":\"%s\",\"params\":\"Q.%s,T.%s\"}",
                        sub_flags[i] ? "subscribe" : "unsubscribe",
                        sub_queue[i], sub_queue[i]);
                    ws_send_text(sub_msg);
                }
                sub_queue_len = 0;
                pthread_mutex_unlock(&sub_mutex);
            }
        }
    }
    cJSON_Delete(root);
}

/* ── Main WS thread ──────────────────────────────────────────────── */

static void *ws_thread_fn(void *arg) {
    (void)arg;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) { fprintf(stderr, "[WS] SSL_CTX_new failed\n"); return NULL; }
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(ssl_ctx);

    while (ws_running) {
        /* connect */
        ws_sock = tcp_connect(WS_HOST, WS_PORT);
        if (ws_sock == SOCK_INVALID) {
            fprintf(stderr, "[WS] TCP connect failed, retrying...\n");
            sleep(5); continue;
        }

        ssl_conn = SSL_new(ssl_ctx);
        SSL_set_fd(ssl_conn, (int)ws_sock);
        SSL_set_tlsext_host_name(ssl_conn, WS_HOST);

        if (SSL_connect(ssl_conn) != 1) {
            fprintf(stderr, "[WS] TLS handshake failed\n");
            goto reconnect;
        }

        if (ws_handshake() != 0) goto reconnect;
        printf("[WS] Connected to Polygon.io\n");

        /* auth */
        char auth_msg[512];
        snprintf(auth_msg, sizeof(auth_msg),
                 "{\"action\":\"auth\",\"params\":\"%s\"}", ws_api_key);
        ws_send_text(auth_msg);

        /* main recv loop */
        char *buf = malloc(RECV_BUF);
        if (!buf) goto reconnect;

        while (ws_running) {
            /* flush pending subscribe requests */
            pthread_mutex_lock(&sub_mutex);
            for (int i = 0; i < sub_queue_len; i++) {
                char sub_msg[256];
                snprintf(sub_msg, sizeof(sub_msg),
                    "{\"action\":\"%s\",\"params\":\"Q.%s,T.%s\"}",
                    sub_flags[i] ? "subscribe" : "unsubscribe",
                    sub_queue[i], sub_queue[i]);
                ws_send_text(sub_msg);
            }
            sub_queue_len = 0;
            pthread_mutex_unlock(&sub_mutex);

            int n = ws_recv_frame(buf, RECV_BUF);
            if (n < 0) { fprintf(stderr, "[WS] Connection closed\n"); break; }
            if (n > 0) process_message(buf);
        }
        free(buf);

    reconnect:
        if (ssl_conn) { SSL_shutdown(ssl_conn); SSL_free(ssl_conn); ssl_conn = NULL; }
        if (ws_sock != SOCK_INVALID) { sock_close(ws_sock); ws_sock = SOCK_INVALID; }
        if (ws_running) { fprintf(stderr, "[WS] Reconnecting in 5s...\n"); sleep(5); }
    }

    SSL_CTX_free(ssl_ctx);
    ssl_ctx = NULL;
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────── */

int polygon_ws_start(const char *api_key) {
    strncpy(ws_api_key, api_key, sizeof(ws_api_key) - 1);
    ws_running = 1;
    return pthread_create(&ws_thread, NULL, ws_thread_fn, NULL);
}

void polygon_ws_subscribe(const char *symbol) {
    pthread_mutex_lock(&sub_mutex);
    if (sub_queue_len < MAX_SYMBOLS) {
        strncpy(sub_queue[sub_queue_len], symbol, MAX_SYMBOL_LEN - 1);
        sub_flags[sub_queue_len] = 1;
        sub_queue_len++;
    }
    pthread_mutex_unlock(&sub_mutex);
}

void polygon_ws_unsubscribe(const char *symbol) {
    pthread_mutex_lock(&sub_mutex);
    if (sub_queue_len < MAX_SYMBOLS) {
        strncpy(sub_queue[sub_queue_len], symbol, MAX_SYMBOL_LEN - 1);
        sub_flags[sub_queue_len] = 0;
        sub_queue_len++;
    }
    pthread_mutex_unlock(&sub_mutex);
}

void polygon_ws_stop(void) {
    ws_running = 0;
    if (ssl_conn) SSL_shutdown(ssl_conn);
    if (ws_sock != SOCK_INVALID) sock_close(ws_sock);
    pthread_join(ws_thread, NULL);
}
