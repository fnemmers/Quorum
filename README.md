# StockApp

A real-time stock market application with a **C backend** and **Java/JavaFX frontend**.

```
┌─────────────────────────────────────────────────────────────┐
│  Java (JavaFX)  ←── TCP/JSON ──►  C Backend                │
│  • Watchlist                      • Polygon.io WebSocket    │
│  • Candlestick chart              • Polygon.io REST         │
│  • Portfolio tracker              • Alert engine            │
│  • Price alerts                   • IPC server :8765        │
└─────────────────────────────────────────────────────────────┘
```

---

## Requirements

### C Backend
| Dependency | Install (MSYS2/MinGW) | Install (Ubuntu/Debian) |
|---|---|---|
| GCC ≥ 11 | `pacman -S mingw-w64-x86_64-gcc` | `apt install gcc` |
| OpenSSL | `pacman -S mingw-w64-x86_64-openssl` | `apt install libssl-dev` |
| libcurl | `pacman -S mingw-w64-x86_64-curl` | `apt install libcurl4-openssl-dev` |
| pthreads | included in MinGW | included in glibc |
| **cJSON** | [download manually](#cjson) | [download manually](#cjson) |

### Java Frontend
| Requirement | Version |
|---|---|
| JDK | 21+ |
| Maven | 3.9+ |

---

## Setup

### 1. Get a Polygon.io API key
Sign up at **https://polygon.io** – the free tier provides 15-minute delayed data.

### 2. Download cJSON  <a id="cjson"></a>
```bash
# from the backend/src/ directory:
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
```

Or download the `.zip` from https://github.com/DaveGamble/cJSON/releases and copy
`cJSON.h` and `cJSON.c` into `backend/src/`.

---

## Build

### C Backend

**MSYS2 / MinGW on Windows:**
```bash
cd stock-app/backend
# Ensure cJSON.h and cJSON.c are in src/
make
# Produces: stock-backend.exe
```

**Linux / macOS:**
```bash
cd stock-app/backend
make
# Produces: stock-backend
```

### Java Frontend
```bash
cd stock-app/frontend
mvn package -DskipTests
# Produces: target/stockapp-frontend-1.0.0.jar
```

---

## Run

### Option A – Java launches C automatically
Put `stock-backend.exe` (or `stock-backend`) in the same directory as the JAR,
or set `STOCK_BACKEND_BIN=/path/to/stock-backend`.

```bash
# Windows (PowerShell / cmd)
set POLYGON_API_KEY=YOUR_KEY_HERE
java -jar target/stockapp-frontend-1.0.0.jar

# Linux / macOS
POLYGON_API_KEY=YOUR_KEY_HERE java -jar target/stockapp-frontend-1.0.0.jar
```

### Option B – Start them separately
```bash
# Terminal 1 – backend
./backend/stock-backend YOUR_POLYGON_API_KEY

# Terminal 2 – frontend
java -jar frontend/target/stockapp-frontend-1.0.0.jar
```

---

## Project Structure

```
stock-app/
├── backend/
│   ├── Makefile
│   └── src/
│       ├── main.c            Entry point
│       ├── market_data.h/c   Shared state, thread-safe ring-buffers
│       ├── polygon_ws.h/c    WebSocket client → Polygon.io (OpenSSL)
│       ├── polygon_rest.h/c  REST client → Polygon.io (libcurl)
│       ├── ipc_server.h/c    Local TCP server on :8765, command dispatcher
│       └── cJSON.h/c         JSON parsing (download separately)
└── frontend/
    ├── pom.xml
    └── src/main/java/com/stockapp/
        ├── Main.java                     App entry point, launches backend
        ├── backend/
        │   └── BackendClient.java        TCP client + event dispatcher
        ├── model/
        │   ├── StockQuote.java           JavaFX observable quote
        │   ├── OHLCBar.java              Candlestick bar
        │   ├── Holding.java              Portfolio position + P&L
        │   └── AlertModel.java           Price alert record
        └── ui/
            ├── QuoteView.java            Watchlist + candlestick chart tab
            ├── CandlestickChart.java     Canvas-based OHLCV chart
            ├── PortfolioView.java        Portfolio tab
            └── AlertView.java           Alerts tab + notification popup
```

---

## IPC Protocol (localhost:8765)

The C backend speaks **newline-delimited JSON** over TCP.

### Java → C (commands)
```json
{"cmd":"subscribe",   "symbol":"AAPL"}
{"cmd":"unsubscribe", "symbol":"AAPL"}
{"cmd":"history",     "symbol":"AAPL","multiplier":1,"timespan":"day","from":"2024-01-01","to":"2024-12-31"}
{"cmd":"snapshot",    "symbol":"AAPL"}
{"cmd":"portfolio_add",    "symbol":"AAPL","shares":10,"price":150.0}
{"cmd":"portfolio_remove", "symbol":"AAPL"}
{"cmd":"portfolio_get"}
{"cmd":"alert_add",    "symbol":"AAPL","condition":"above","price":160.0}
{"cmd":"alert_remove", "id":3}
{"cmd":"alert_list"}
```

### C → Java (events)
```json
{"type":"quote",     "symbol":"AAPL","price":150.25,"bid":150.20,"ask":150.30,"volume":123456,"ts":1700000000000}
{"type":"history",   "symbol":"AAPL","bars":[{"t":...,"o":...,"h":...,"l":...,"c":...,"v":...}]}
{"type":"alert",     "id":3,"symbol":"AAPL","condition":"above","trigger":160.0,"price":161.5}
{"type":"portfolio", "holdings":[{"symbol":"AAPL","shares":10,"avg_price":150.0,"current":161.5}]}
{"type":"alert_list","alerts":[{"id":3,"symbol":"AAPL","condition":"above","price":160.0}]}
{"type":"error",     "message":"..."}
```

---

## Performance Notes

- **C ring-buffer**: 1 000 prices per symbol kept in memory; O(1) updates.
- **Thread model**: WS thread, REST worker, IPC accept thread, one thread per client.
  All state access serialised through a single `pthread_mutex_t`.
- **Java FX thread**: All quote updates dispatched via `Platform.runLater()`.
  Chart redraws are deferred to avoid frame drops during high-frequency ticks.
- **Chart**: Pure Canvas – no external chart library. Renders up to 200 bars,
  uses hardware-accelerated JavaFX renderer.

---

## Extending

| What | Where |
|---|---|
| Add more Polygon event types (trades, minute aggs) | `polygon_ws.c → process_message()` |
| Persist portfolio to disk | `market_data.c` + JSON file I/O |
| Add a mini price-history sparkline | `CandlestickChart.java` (second canvas) |
| Paper-trading order simulation | new `orders.c` + `OrderView.java` |
| Volume histogram below chart | extend `CandlestickChart.redraw()` |
