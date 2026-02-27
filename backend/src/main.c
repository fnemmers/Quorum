/*
 * stock-app backend – main entry point
 *
 * Usage:  stock-backend <POLYGON_API_KEY> [PG_PASSWORD]
 *
 *   PG_PASSWORD defaults to "postgres" if not supplied.
 *   You can also set the environment variable PGPASSWORD.
 *
 * Starts:
 *   1. PostgreSQL database connection
 *   2. Market data state (loads saved portfolio + alerts from DB)
 *   3. IPC TCP server (port 8765)  ← Java connects here
 *   4. Polygon.io REST client      ← historical data on demand
 *   5. Polygon.io WebSocket client ← real-time streaming
 *
 * The process runs until SIGINT/SIGTERM.
 */

#include "market_data.h"
#include "ipc_server.h"
#include "polygon_ws.h"
#include "polygon_rest.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

static volatile int g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <POLYGON_API_KEY> [DB_PASSWORD]\n"
            "  DB_PASSWORD defaults to 'postgres'\n",
            argv[0]);
        return 1;
    }

    const char *api_key  = argv[1];
    const char *pg_pass  = (argc >= 3) ? argv[2] : "postgres";

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[MAIN] Initialising stock-app backend...\n");

    /* ── PostgreSQL ─────────────────────────────────────────── */
    char connstr[512];
    snprintf(connstr, sizeof(connstr),
        "host=localhost dbname=stockapp user=postgres password=%s",
        pg_pass);

    if (db_init(connstr) != 0) {
        fprintf(stderr,
            "[MAIN] Failed to connect to PostgreSQL.\n"
            "       Is the server running? (pg_ctl start)\n"
            "       Connection string: %s\n", connstr);
        return 1;
    }

    /* ── Market state (loads portfolio + alerts from DB) ────── */
    market_data_init();
    db_portfolio_load();
    db_alerts_load();

    /* ── REST + IPC + WebSocket ─────────────────────────────── */
    polygon_rest_init(api_key);

    if (ipc_server_start() != 0) {
        fprintf(stderr, "[MAIN] Failed to start IPC server\n");
        return 1;
    }

    if (polygon_ws_start(api_key) != 0) {
        fprintf(stderr, "[MAIN] Failed to start WebSocket client\n");
        return 1;
    }

    printf("[MAIN] Backend ready. Waiting for Java client on port 8765...\n");

    while (g_running) {
#ifdef _WIN32
        Sleep(500);
#else
        usleep(500000);
#endif
    }

    printf("[MAIN] Shutting down...\n");
    polygon_ws_stop();
    ipc_server_stop();
    polygon_rest_cleanup();
    db_close();
    return 0;
}
