# ─────────────────────────────────────────────────────────────────────────────
# quorum C backend Makefile
#
# Dependencies (MSYS2 MinGW 64-bit):
#   pacman -S mingw-w64-x86_64-gcc
#             mingw-w64-x86_64-openssl
#             mingw-w64-x86_64-curl
#             mingw-w64-x86_64-postgresql
#
# cJSON (manual download into backend/src/):
#   curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
#   curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
#
# Run:
#   ./quorum-backend.exe YOUR_POLYGON_API_KEY [DB_PASSWORD]
#   DB_PASSWORD defaults to "postgres"
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -Isrc
LDFLAGS =

# Platform detection
ifeq ($(OS),Windows_NT)
    # PostgreSQL include/lib paths (MSYS2 MinGW)
    PG_INC  = $(shell pg_config --includedir 2>/dev/null || echo /mingw64/include)
    PG_LIB  = $(shell pg_config --libdir     2>/dev/null || echo /mingw64/lib)

    CFLAGS  += -D_WIN32_WINNT=0x0601 -I$(PG_INC)
    LDFLAGS += -L$(PG_LIB) -lws2_32 -lssl -lcrypto -lcurl -lpq -lpthread -lm
    TARGET   = quorum-backend.exe
else
    PG_INC    = $(shell /opt/homebrew/opt/postgresql@17/bin/pg_config --includedir 2>/dev/null || echo /opt/homebrew/include/postgresql@17)
    PG_LIB    = $(shell /opt/homebrew/opt/postgresql@17/bin/pg_config --libdir    2>/dev/null || echo /opt/homebrew/lib/postgresql@17)
    SSL_INC   = $(shell brew --prefix openssl 2>/dev/null)/include
    SSL_LIB   = $(shell brew --prefix openssl 2>/dev/null)/lib
    CFLAGS   += -I$(PG_INC) -I$(SSL_INC)
    LDFLAGS  += -L$(SSL_LIB) -L$(PG_LIB) -lssl -lcrypto -lcurl -lpq -lpthread -lm
    TARGET    = quorum-backend
endif

SRCS = src/main.c            \
       src/market_data.c     \
       src/polygon_ws.c      \
       src/polygon_rest.c    \
       src/ipc_server.c      \
       src/ipc_research.c    \
       src/db.c              \
       src/bot_picks.c       \
       src/aggregation.c     \
       src/backtest.c        \
       src/convergence.c     \
       src/sp500_universe.c  \
       src/crawler.c         \
       src/heston.c          \
       src/heston_surface.c  \
       src/risk_score.c      \
       src/rebalance.c       \
       src/cJSON.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: check-cjson $(TARGET)

check-cjson:
	@if [ ! -f src/cJSON.c ] || [ ! -f src/cJSON.h ]; then \
	    echo ""; \
	    echo "ERROR: cJSON not found in src/"; \
	    echo "Run from backend/src/:"; \
	    echo "  curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h"; \
	    echo "  curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c"; \
	    echo ""; \
	    exit 1; \
	fi

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "Build complete: $@"
	@echo "Run: ./$@ YOUR_POLYGON_API_KEY [DB_PASSWORD]"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
