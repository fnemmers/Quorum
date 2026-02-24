#ifndef IPC_SERVER_H
#define IPC_SERVER_H

/*
 * ipc_server  –  Local TCP server on port 8765.
 *
 * Java connects here and sends JSON commands.
 * The server broadcasts JSON events back to all connected clients.
 *
 * Command → Response protocol (newline-delimited JSON):
 *
 * Java → C (commands):
 *   {"cmd":"subscribe",  "symbol":"AAPL"}
 *   {"cmd":"unsubscribe","symbol":"AAPL"}
 *   {"cmd":"history",    "symbol":"AAPL","multiplier":1,
 *                         "timespan":"day","from":"2024-01-01","to":"2024-12-31"}
 *   {"cmd":"snapshot",   "symbol":"AAPL"}
 *   {"cmd":"portfolio_add",    "symbol":"AAPL","shares":10,"price":150.0}
 *   {"cmd":"portfolio_remove", "symbol":"AAPL"}
 *   {"cmd":"portfolio_get"}
 *   {"cmd":"alert_add",    "symbol":"AAPL","condition":"above","price":160.0}
 *   {"cmd":"alert_remove", "id":3}
 *   {"cmd":"alert_list"}
 *
 * C → Java (events/responses):
 *   {"type":"quote",     "symbol":"AAPL","price":150.25,"bid":150.20,"ask":150.30,...}
 *   {"type":"history",   "symbol":"AAPL","bars":[{"t":...,"o":...,"h":...,"l":...,"c":...,"v":...},...]}
 *   {"type":"alert",     "id":3,"symbol":"AAPL","condition":"above","trigger":160.0,"price":161.0}
 *   {"type":"portfolio", "holdings":[{"symbol":"AAPL","shares":10,"avg":150.0,"current":161.0},...]}
 *   {"type":"alert_list","alerts":[{"id":3,"symbol":"AAPL","condition":"above","price":160.0},...]}
 *   {"type":"error",     "message":"..."}
 */

#define IPC_PORT 8765

/* Start the IPC server thread.  Returns 0 on success. */
int ipc_server_start(void);

/* Stop the IPC server and wait for thread to exit. */
void ipc_server_stop(void);

/* Broadcast a NUL-terminated JSON string (with trailing '\n') to all clients.
   Thread-safe.  Called from WS thread and REST worker. */
void ipc_broadcast(const char *json_line);

#endif /* IPC_SERVER_H */
