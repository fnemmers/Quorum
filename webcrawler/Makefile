# ── Makefile ──────────────────────────────────────────────────────────────────
CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread
INCLUDES := -I include

# Detect libcurl via pkg-config; fall back to -lcurl
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell pkg-config --libs   libcurl 2>/dev/null || echo "-lcurl")

CRAWLER_BIN        := crawler
INDEXER_BIN        := indexer
QUERY_BIN          := query
MARKET_CRAWLER_BIN := market_crawler
SIGNAL_ENGINE_BIN  := signal_engine
NEWS_CRAWLER_BIN   := news_crawler

HEADERS     := $(wildcard include/*.h)

.PHONY: all clean dirs run market-run

all: $(CRAWLER_BIN) $(INDEXER_BIN) $(QUERY_BIN) \
     $(MARKET_CRAWLER_BIN) $(SIGNAL_ENGINE_BIN) $(NEWS_CRAWLER_BIN)

# ── Original web-crawler pipeline ─────────────────────────────────────────────
$(CRAWLER_BIN): src/crawler.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CURL_CFLAGS) $(INCLUDES) -o $@ $< $(CURL_LIBS) -lpthread

$(INDEXER_BIN): src/indexer.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -lpthread

$(QUERY_BIN): src/query.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -lpthread

# ── Market data pipeline ───────────────────────────────────────────────────────
$(MARKET_CRAWLER_BIN): src/market_crawler.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CURL_CFLAGS) $(INCLUDES) -o $@ $< $(CURL_LIBS) -lpthread

$(SIGNAL_ENGINE_BIN): src/signal_engine.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -lpthread

$(NEWS_CRAWLER_BIN): src/news_crawler.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CURL_CFLAGS) $(INCLUDES) -o $@ $< $(CURL_LIBS) -lpthread

dirs:
	mkdir -p data/pages data/index data/market data/news

# ── Quick smoke-test run (web crawler) ────────────────────────────────────────
IPC_SOCK  := /tmp/crawler_ipc.sock
SEED      := https://en.wikipedia.org/wiki/Web_crawler
MAX_DEPTH := 1
MAX_PAGES := 20
THREADS   := 4

run: all dirs
	@echo ">>> Starting indexer..."
	./$(INDEXER_BIN) --ipc $(IPC_SOCK) --out data &
	@sleep 0.3
	@echo ">>> Starting crawler (seed=$(SEED))..."
	./$(CRAWLER_BIN) \
		--seed      "$(SEED)" \
		--max-depth $(MAX_DEPTH) \
		--max-pages $(MAX_PAGES) \
		-t          $(THREADS) \
		--out       data \
		--ipc       $(IPC_SOCK)
	@echo ""
	@echo ">>> Crawl complete. Try:"
	@echo "    ./$(QUERY_BIN) --index data/index web crawler"

# ── Market data + signal pipeline ─────────────────────────────────────────────
# Set POLYGON_KEY before running:  make market-run POLYGON_KEY=<your_key>
POLYGON_KEY    ?= 3ODCPPUW3ZXNHXYV
MARKET_IPC     := /tmp/market_ipc.sock
MARKET_TICKERS := AAPL,MSFT,GOOGL,AMZN,NVDA
MARKET_FROM    := 2025-01-01
MARKET_TO      := 2026-04-19
MARKET_OUT     := data/market

market-run: all dirs
	@echo ">>> Starting signal engine (IPC mode)..."
	./$(SIGNAL_ENGINE_BIN) --ipc $(MARKET_IPC) &
	@sleep 0.3
	@echo ">>> Starting market crawler (tickers=$(MARKET_TICKERS))..."
	./$(MARKET_CRAWLER_BIN) \
		--tickers   $(MARKET_TICKERS) \
		--api-key   $(POLYGON_KEY)    \
		--from      $(MARKET_FROM)    \
		--to        $(MARKET_TO)      \
		--out       $(MARKET_OUT)     \
		-t          1                 \
		--delay-ms  15000             \
		--ipc       $(MARKET_IPC)
	@echo ""
	@echo ">>> Market run complete.  Snapshots: $(MARKET_OUT)/"
	@echo "    Batch re-run:  ./$(SIGNAL_ENGINE_BIN) --in $(MARKET_OUT)"

# ── News crawl + full signal run ──────────────────────────────────────────────
NEWS_OUT := data/news

news-run: all dirs
	@echo ">>> Crawling live financial news (Yahoo Finance + Finviz)..."
	./$(NEWS_CRAWLER_BIN) \
		--tickers  $(MARKET_TICKERS) \
		--out      $(NEWS_OUT)       \
		-t         2                 \
		--delay-ms 1500
	@echo ""
	@echo ">>> Running signals with live sentiment..."
	./$(SIGNAL_ENGINE_BIN) \
		--in       $(MARKET_OUT) \
		--news-dir $(NEWS_OUT)

clean:
	rm -f $(CRAWLER_BIN) $(INDEXER_BIN) $(QUERY_BIN) \
	      $(MARKET_CRAWLER_BIN) $(SIGNAL_ENGINE_BIN) $(NEWS_CRAWLER_BIN)
	rm -rf data/
