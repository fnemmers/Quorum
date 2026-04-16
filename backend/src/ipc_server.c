#include "ipc_server.h"
#include "ipc_research.h"
#include "market_data.h"
#include "polygon_ws.h"
#include "polygon_rest.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID  INVALID_SOCKET
  #define sock_close(s) closesocket(s)
  #define sock_errno()  WSAGetLastError()
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int sock_t;
  #define SOCK_INVALID  -1
  #define sock_close(s) close(s)
  #define sock_errno()  errno
#endif

/* ── Shared client table ─────────────────────────────────────────── */

static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
static sock_t client_fds[MAX_CLIENTS];
static int    client_count = 0;

static volatile int ipc_running = 0;
static pthread_t    ipc_thread;
static sock_t       listen_sock = SOCK_INVALID;

/* ── Broadcast ──────────────────────────────────────────────────── */

void ipc_broadcast(const char *json_line) {
    pthread_mutex_lock(&client_mutex);
    for (int i = 0; i < client_count; ) {
        int n = send(client_fds[i], json_line, (int)strlen(json_line), 0);
        if (n <= 0) {
            sock_close(client_fds[i]);
            client_fds[i] = client_fds[--client_count];
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

/* ── Send to single client ──────────────────────────────────────── */

static void client_send(sock_t fd, const char *json_line) {
    send(fd, json_line, (int)strlen(json_line), 0);
}

/* ── Command handlers ───────────────────────────────────────────── */

static void cmd_subscribe(sock_t fd, cJSON *root) {
    cJSON *sym = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    if (!sym || !cJSON_IsString(sym)) {
        client_send(fd, "{\"type\":\"error\",\"message\":\"missing symbol\"}\n");
        return;
    }
    market_get_or_add_symbol(sym->valuestring);
    polygon_ws_subscribe(sym->valuestring);
    printf("[IPC] Subscribe: %s\n", sym->valuestring);
}

static void cmd_unsubscribe(sock_t fd, cJSON *root) {
    cJSON *sym = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    if (!sym || !cJSON_IsString(sym)) return;
    polygon_ws_unsubscribe(sym->valuestring);
}

static void cmd_history(sock_t fd, cJSON *root) {
    cJSON *sym  = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    cJSON *mult = cJSON_GetObjectItemCaseSensitive(root, "multiplier");
    cJSON *ts   = cJSON_GetObjectItemCaseSensitive(root, "timespan");
    cJSON *from = cJSON_GetObjectItemCaseSensitive(root, "from");
    cJSON *to   = cJSON_GetObjectItemCaseSensitive(root, "to");

    if (!sym || !cJSON_IsString(sym)) {
        client_send(fd, "{\"type\":\"error\",\"message\":\"missing symbol\"}\n");
        return;
    }

    int   m    = mult && cJSON_IsNumber(mult) ? (int)mult->valuedouble : 1;
    const char *tspan = ts   && cJSON_IsString(ts)   ? ts->valuestring   : "day";
    const char *f     = from && cJSON_IsString(from)  ? from->valuestring : "2024-01-01";
    const char *t     = to   && cJSON_IsString(to)    ? to->valuestring   : "2024-12-31";

    OHLCBar *bars = malloc(sizeof(OHLCBar) * 5000);
    if (!bars) return;

    int count = polygon_rest_aggregates(sym->valuestring, m, tspan, f, t, bars, 5000);

    if (count < 0) {
        client_send(fd, "{\"type\":\"error\",\"message\":\"history fetch failed\"}\n");
        free(bars);
        return;
    }

    /* Build JSON response */
    cJSON *resp  = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type",   "history");
    cJSON_AddStringToObject(resp, "symbol", sym->valuestring);
    cJSON *arr = cJSON_AddArrayToObject(resp, "bars");
    for (int i = 0; i < count; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "t", (double)bars[i].timestamp);
        cJSON_AddNumberToObject(b, "o", bars[i].open);
        cJSON_AddNumberToObject(b, "h", bars[i].high);
        cJSON_AddNumberToObject(b, "l", bars[i].low);
        cJSON_AddNumberToObject(b, "c", bars[i].close);
        cJSON_AddNumberToObject(b, "v", (double)bars[i].volume);
        cJSON_AddItemToArray(arr, b);
    }
    free(bars);

    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (str) {
        /* append newline and send */
        size_t len = strlen(str);
        char *line = malloc(len + 2);
        memcpy(line, str, len);
        line[len]   = '\n';
        line[len+1] = '\0';
        client_send(fd, line);
        free(line);
        free(str);
    }
}

static void cmd_snapshot(sock_t fd, cJSON *root) {
    cJSON *sym = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    if (!sym || !cJSON_IsString(sym)) return;

    Quote q = {0};
    if (polygon_rest_snapshot(sym->valuestring, &q) == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
            "{\"type\":\"quote\",\"symbol\":\"%s\","
            "\"price\":%.4f,\"bid\":%.4f,\"ask\":%.4f,"
            "\"volume\":%lld,\"ts\":%lld}\n",
            q.symbol, q.price, q.bid, q.ask,
            (long long)q.volume, (long long)q.timestamp);
        client_send(fd, msg);
    } else {
        client_send(fd, "{\"type\":\"error\",\"message\":\"snapshot failed\"}\n");
    }
}

static void cmd_portfolio_add(sock_t fd, cJSON *root) {
    cJSON *sym    = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    cJSON *shares = cJSON_GetObjectItemCaseSensitive(root, "shares");
    cJSON *price  = cJSON_GetObjectItemCaseSensitive(root, "price");
    if (!sym || !cJSON_IsString(sym)) return;
    double s = shares && cJSON_IsNumber(shares) ? shares->valuedouble : 0;
    double p = price  && cJSON_IsNumber(price)  ? price->valuedouble  : 0;
    market_portfolio_add(sym->valuestring, s, p);
}

static void cmd_portfolio_remove(sock_t fd, cJSON *root) {
    cJSON *sym = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    if (sym && cJSON_IsString(sym))
        market_portfolio_remove(sym->valuestring);
}

static void cmd_portfolio_get(sock_t fd) {
    pthread_mutex_lock(&g_state.lock);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "portfolio");
    cJSON *arr = cJSON_AddArrayToObject(resp, "holdings");

    for (int i = 0; i < g_state.holding_count; i++) {
        Holding *h = &g_state.holdings[i];
        /* find current price */
        double cur = 0.0;
        int idx = market_find_symbol(h->symbol);
        if (idx >= 0 && g_state.quotes[idx].valid)
            cur = g_state.quotes[idx].price;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "symbol", h->symbol);
        cJSON_AddNumberToObject(item, "shares",    h->shares);
        cJSON_AddNumberToObject(item, "avg_price", h->avg_price);
        cJSON_AddNumberToObject(item, "current",   cur);
        cJSON_AddItemToArray(arr, item);
    }
    pthread_mutex_unlock(&g_state.lock);

    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (str) {
        size_t len = strlen(str);
        char *line = malloc(len + 2);
        memcpy(line, str, len);
        line[len] = '\n'; line[len+1] = '\0';
        client_send(fd, line);
        free(line); free(str);
    }
}

static void cmd_alert_add(sock_t fd, cJSON *root) {
    cJSON *sym  = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    cJSON *cond = cJSON_GetObjectItemCaseSensitive(root, "condition");
    cJSON *pr   = cJSON_GetObjectItemCaseSensitive(root, "price");
    if (!sym || !cJSON_IsString(sym) || !cond || !cJSON_IsString(cond)) return;
    AlertType type = strcmp(cond->valuestring, "above") == 0 ? ALERT_ABOVE : ALERT_BELOW;
    double trigger = pr && cJSON_IsNumber(pr) ? pr->valuedouble : 0.0;
    int id = market_alert_add(sym->valuestring, type, trigger);
    char msg[256];
    snprintf(msg, sizeof(msg),
        "{\"type\":\"alert_added\",\"id\":%d,\"symbol\":\"%s\","
        "\"condition\":\"%s\",\"price\":%.4f}\n",
        id, sym->valuestring, cond->valuestring, trigger);
    client_send(fd, msg);
}

static void cmd_alert_remove(sock_t fd, cJSON *root) {
    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (id_j && cJSON_IsNumber(id_j))
        market_alert_remove((int)id_j->valuedouble);
}

static void cmd_alert_list(sock_t fd) {
    pthread_mutex_lock(&g_state.lock);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "alert_list");
    cJSON *arr = cJSON_AddArrayToObject(resp, "alerts");
    for (int i = 0; i < g_state.alert_count; i++) {
        AlertRecord *a = &g_state.alerts[i];
        if (!a->active) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id",        a->id);
        cJSON_AddStringToObject(item, "symbol",    a->symbol);
        cJSON_AddStringToObject(item, "condition",
                                a->type == ALERT_ABOVE ? "above" : "below");
        cJSON_AddNumberToObject(item, "price",     a->trigger_price);
        cJSON_AddItemToArray(arr, item);
    }
    pthread_mutex_unlock(&g_state.lock);
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (str) {
        size_t len = strlen(str);
        char *line = malloc(len + 2);
        memcpy(line, str, len);
        line[len] = '\n'; line[len+1] = '\0';
        client_send(fd, line);
        free(line); free(str);
    }
}

/* ── Per-client handler thread ──────────────────────────────────── */

static void *client_thread_fn(void *arg) {
    sock_t fd = (sock_t)(intptr_t)arg;

    /* Read loop – commands are newline-delimited */
    char  line[4096];
    int   pos = 0;
    char  ch;

    while (ipc_running) {
        int n = recv(fd, &ch, 1, 0);
        if (n <= 0) break;
        if (ch == '\n' || pos >= (int)sizeof(line) - 1) {
            line[pos] = '\0';
            pos = 0;
            if (line[0] == '\0') continue;

            cJSON *root = cJSON_Parse(line);
            if (!root) continue;
            cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(root, "cmd");
            if (cmd_j && cJSON_IsString(cmd_j)) {
                const char *cmd = cmd_j->valuestring;
                if      (!strcmp(cmd, "subscribe"))       cmd_subscribe(fd, root);
                else if (!strcmp(cmd, "unsubscribe"))     cmd_unsubscribe(fd, root);
                else if (!strcmp(cmd, "history"))         cmd_history(fd, root);
                else if (!strcmp(cmd, "snapshot"))        cmd_snapshot(fd, root);
                else if (!strcmp(cmd, "portfolio_add"))   cmd_portfolio_add(fd, root);
                else if (!strcmp(cmd, "portfolio_remove"))cmd_portfolio_remove(fd, root);
                else if (!strcmp(cmd, "portfolio_get"))   cmd_portfolio_get(fd);
                else if (!strcmp(cmd, "alert_add"))       cmd_alert_add(fd, root);
                else if (!strcmp(cmd, "alert_remove"))    cmd_alert_remove(fd, root);
                else if (!strcmp(cmd, "alert_list"))      cmd_alert_list(fd);
                else {
                    /* Hand off to the research module (bots, aggregation,
                     * backtest). It returns 1 if it handled the command. */
                    if (!ipc_research_dispatch((int)fd, cmd, root)) {
                        char err[128];
                        snprintf(err, sizeof(err),
                            "{\"type\":\"error\",\"message\":\"unknown cmd: %.40s\"}\n",
                            cmd);
                        client_send(fd, err);
                    }
                }
            }
            cJSON_Delete(root);
        } else {
            line[pos++] = ch;
        }
    }

    /* remove from client table */
    pthread_mutex_lock(&client_mutex);
    for (int i = 0; i < client_count; i++) {
        if (client_fds[i] == fd) {
            client_fds[i] = client_fds[--client_count];
            break;
        }
    }
    pthread_mutex_unlock(&client_mutex);
    sock_close(fd);
    printf("[IPC] Client disconnected\n");
    return NULL;
}

/* ── Accept loop ────────────────────────────────────────────────── */

static void *ipc_thread_fn(void *arg) {
    (void)arg;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(IPC_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_sock, 8);

    printf("[IPC] Listening on port %d\n", IPC_PORT);

    while (ipc_running) {
        struct sockaddr_in ca;
        socklen_t cal = sizeof(ca);
        sock_t cfd = accept(listen_sock, (struct sockaddr *)&ca, &cal);
        if (cfd == SOCK_INVALID) break;

        printf("[IPC] Client connected\n");

        pthread_mutex_lock(&client_mutex);
        if (client_count < MAX_CLIENTS)
            client_fds[client_count++] = cfd;
        pthread_mutex_unlock(&client_mutex);

        pthread_t t;
        pthread_create(&t, NULL, client_thread_fn, (void *)(intptr_t)cfd);
        pthread_detach(t);
    }
    return NULL;
}

/* ── Public ─────────────────────────────────────────────────────── */

int ipc_server_start(void) {
    ipc_running = 1;
    return pthread_create(&ipc_thread, NULL, ipc_thread_fn, NULL);
}

void ipc_server_stop(void) {
    ipc_running = 0;
    if (listen_sock != SOCK_INVALID) sock_close(listen_sock);
    pthread_join(ipc_thread, NULL);
}
