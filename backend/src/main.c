/*
 * quorum backend main entry point
 *
 * Usage:  quorum-backend <POLYGON_API_KEY>
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
#include "crawler.h"
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
        fprintf(stderr, "Usage: %s <POLYGON_API_KEY> [DB_PASSWORD]\n", argv[0]);
        return 1;
    }
    const char *api_key     = argv[1];
    const char *db_password = (argc >= 3) ? argv[2] : NULL;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[MAIN] Initialising quorum backend...\n");

    market_data_init();
    polygon_rest_init(api_key);

    /* Connect to Postgres. The connection string can be overridden via
     * the PG_CONNSTR env var; defaults to a local dev DB with the
     * optional password from argv[2] (passed via make run from .env's
     * DB_PASSWORD). The schema is auto-created on first connection.
     *
     * NOTE: password is embedded in the connstr at runtime only — it
     * is never logged. PGconnectdb is documented to scrub the input. */
    {
        char buf[512];
        const char *connstr = getenv("PG_CONNSTR");
        if (!connstr || !*connstr) {
            if (db_password && *db_password) {
                snprintf(buf, sizeof(buf),
                    "host=localhost dbname=stockapp user=postgres password=%s",
                    db_password);
            } else {
                snprintf(buf, sizeof(buf),
                    "host=localhost dbname=stockapp user=postgres");
            }
            connstr = buf;
        }
        if (db_init(connstr) != 0) {
            fprintf(stderr, "[MAIN] db_init failed — research IPC commands "
                            "will not work. Continuing anyway.\n");
        }
    }

    crawler_init(api_key);
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
