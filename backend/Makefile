# ─────────────────────────────────────────────────────────────────────────────
# stock-app C backend Makefile
#
# Dependencies (install via MSYS2/pacman or apt/brew):
#   - OpenSSL  : libssl-dev  / openssl
#   - libcurl  : libcurl4-openssl-dev / curl
#   - cJSON    : https://github.com/DaveGamble/cJSON  (put cJSON.h + cJSON.c here)
#   - pthreads : included on Linux; mingw-w64-x86_64-winpthreads on Windows
#
# MSYS2 one-liner:
#   pacman -S mingw-w64-x86_64-openssl mingw-w64-x86_64-curl mingw-w64-x86_64-pthreads
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -Isrc
LDFLAGS =

# Platform detection
ifeq ($(OS),Windows_NT)
    CFLAGS  += -D_WIN32_WINNT=0x0601
    LDFLAGS += -lws2_32 -lssl -lcrypto -lcurl -lpthread
    TARGET   = stock-backend.exe
else
    LDFLAGS += -lssl -lcrypto -lcurl -lpthread
    TARGET   = stock-backend
endif

SRCS = src/main.c          \
       src/market_data.c   \
       src/polygon_ws.c    \
       src/polygon_rest.c  \
       src/ipc_server.c    \
       src/cJSON.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: check-cjson $(TARGET)

# Remind user to download cJSON if missing
check-cjson:
	@if [ ! -f src/cJSON.c ] || [ ! -f src/cJSON.h ]; then \
	    echo ""; \
	    echo "ERROR: cJSON not found in src/"; \
	    echo "Download from: https://github.com/DaveGamble/cJSON/releases"; \
	    echo "Copy cJSON.h and cJSON.c into backend/src/"; \
	    echo ""; \
	    exit 1; \
	fi

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "Build complete: $@"
	@echo "Run: ./$@ YOUR_POLYGON_API_KEY"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
