# ─────────────────────────────────────────────────────────────────────────────
# stock-app C backend Makefile
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
#   ./stock-backend.exe YOUR_POLYGON_API_KEY [DB_PASSWORD]
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
    LDFLAGS += -L$(PG_LIB) -lws2_32 -lssl -lcrypto -lcurl -lpq -lpthread
    TARGET   = stock-backend.exe
else
    PG_INC  = $(shell pg_config --includedir 2>/dev/null || echo /usr/include/postgresql)
    CFLAGS  += -I$(PG_INC)
    LDFLAGS += -lssl -lcrypto -lcurl -lpq -lpthread
    TARGET   = stock-backend
endif

SRCS = src/main.c          \
       src/market_data.c   \
       src/polygon_ws.c    \
       src/polygon_rest.c  \
       src/ipc_server.c    \
       src/db.c            \
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
