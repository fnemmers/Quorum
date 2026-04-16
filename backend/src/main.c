/*
 * stock-app backend main entry point
 *
 * Usage:  stock-backend <POLYGON_API_KEY>
 *
 * Starts:
 *   1. Market data state
 *   2. IPC TCP server (port 8765)  ← Java connects here
 *   3. Polygon.io REST client       ← historical data on demand
 *   4. Polygon.io WebSocket client  ← real-time streaming
 *
 * The process runs until SIGINT/SIGTERM.
 */

#include "market_data.h"
#include "ipc_server.h"
#include "ipc_research.h"
#include "polygon_ws.h"
#include "polygon_rest.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifndef _WIN32
#include <unistd.h>
#endif

static volatile int g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <POLYGON_API_KEY>\n", argv[0]);
        return 1;
    }
    const char *api_key = argv[1];

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[MAIN] Initialising stock-app backend...\n");

    market_data_init();
    polygon_rest_init(api_key);

    /* Connect to Postgres. The connection string can be overridden via
     * the PG_CONNSTR env var; defaults to a local dev DB. The schema
     * (including the new bot_runs/bot_picks/backtest_results tables) is
     * created on first run. */
    {
        const char *connstr = getenv("PG_CONNSTR");
        if (!connstr || !*connstr)
            connstr = "host=localhost dbname=stockapp user=postgres";
        if (db_init(connstr) != 0) {
            fprintf(stderr, "[MAIN] db_init failed — research IPC commands "
                            "will not work. Continuing anyway.\n");
        }
    }

    ipc_research_init();

    if (ipc_server_start() != 0) {
        fprintf(stderr, "[MAIN] Failed to start IPC server\n");
        return 1;
    }

    if (polygon_ws_start(api_key) != 0) {
        fprintf(stderr, "[MAIN] Failed to start WebSocket client\n");
        return 1;
    }

    printf("[MAIN] Backend ready.  Waiting for Java client on port 8765...\n");

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
    ipc_research_cleanup();
    polygon_rest_cleanup();
    db_close();
    return 0;
}
