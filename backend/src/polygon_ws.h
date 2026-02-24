#ifndef POLYGON_WS_H
#define POLYGON_WS_H

/*
 * polygon_ws  –  WebSocket client for Polygon.io real-time feed.
 *
 * Uses OpenSSL for TLS.  Polygon.io WebSocket endpoint:
 *   wss://delayed.polygon.io/stocks   (free tier – 15-min delayed)
 *   wss://socket.polygon.io/stocks    (paid tier – real-time)
 *
 * After connecting and authenticating, call polygon_ws_subscribe()
 * with each symbol you want to stream.  Incoming messages are
 * decoded and fed into market_update_quote() then broadcast to
 * IPC clients automatically.
 */

/* Start the WebSocket thread – returns 0 on success */
int polygon_ws_start(const char *api_key);

/* Subscribe/unsubscribe to a symbol (thread-safe) */
void polygon_ws_subscribe(const char *symbol);
void polygon_ws_unsubscribe(const char *symbol);

/* Signal the WS thread to stop and wait for it */
void polygon_ws_stop(void);

#endif /* POLYGON_WS_H */
