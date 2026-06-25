# Top-level Makefile — builds every component of quorum.
#
# Usage:
#   make              # build backend + install bridge/bot deps + build frontend
#   make backend      # C backend only
#   make frontend     # React production bundle (frontend-react/dist)
#   make bridge       # install Node bridge deps
#   make bots         # install Python bot deps
#   make vllm         # launch the local vLLM OpenAI server on :8000
#   make clean        # remove all build artifacts
#   make run          # launch backend + bridge + frontend dev server together
#                     # (requires .env with POLYGON_API_KEY; Ctrl-C stops all three)
#                     # Run `make vllm` separately before invoking the bots.

# vLLM defaults — override at the command line or in your shell:
#   VLLM_MODEL=path/to/local/weights make vllm
VLLM_MODEL    ?= Qwen/Qwen2.5-14B-Instruct-AWQ
VLLM_PORT     ?= 8000
VLLM_GPU_UTIL ?= 0.85
VLLM_CTX_LEN  ?= 8192

# Pick the right backend binary name per platform.
ifeq ($(OS),Windows_NT)
    BACKEND_BIN = ./quorum-backend.exe
else
    BACKEND_BIN = ./quorum-backend
endif

.PHONY: all backend frontend bridge bots vllm clean run

all: backend bridge bots frontend
	@echo ""
	@echo "──────────────────────────────────────────────"
	@echo " Build complete. Next: fill .env, then 'make run'."
	@echo "──────────────────────────────────────────────"

backend:
	@echo "[build] C backend"
	$(MAKE) -C backend

frontend:
	@echo "[build] React frontend (production bundle)"
	cd frontend-react && npm install && npm run build

bridge:
	@echo "[build] Node bridge deps"
	cd bridge && npm install

bots:
	@echo "[build] Python bot deps"
	cd bots && ( [ -d venv ] || python3 -m venv venv ) && \
		./venv/bin/pip install -r requirements.txt

# Launch vLLM with the persona-bot defaults. Override VLLM_MODEL with a
# local weights path or a different HF repo. --enable-prefix-caching is
# what lets 100+ bots share the system-prompt KV cache for free.
vllm:
	@command -v vllm >/dev/null 2>&1 || (echo "ERROR: 'vllm' not on PATH. Install via 'pip install vllm' inside the venv you launch from."; exit 1)
	@echo "[vllm] Serving $(VLLM_MODEL) on :$(VLLM_PORT)"
	vllm serve $(VLLM_MODEL) \
		--host 0.0.0.0 --port $(VLLM_PORT) \
		--gpu-memory-utilization $(VLLM_GPU_UTIL) \
		--max-model-len $(VLLM_CTX_LEN) \
		--enable-prefix-caching

clean:
	$(MAKE) -C backend clean
	rm -rf frontend-react/dist frontend-react/node_modules
	rm -rf bridge/node_modules
	@echo "[clean] done (Python venv left intact — remove bots/venv manually if desired)"

# Launch all three long-running processes. Backend reads POLYGON_API_KEY and
# DB_PASSWORD from .env. Ctrl-C traps SIGINT and kills all three.
run:
	@test -f .env || (echo "ERROR: .env not found. Create one and fill in POLYGON_API_KEY."; exit 1)
	@set -a; . ./.env; set +a; \
	  test -n "$$POLYGON_API_KEY" || (echo "ERROR: POLYGON_API_KEY is empty in .env"; exit 1); \
	  trap 'kill 0' INT TERM; \
	  ( cd backend      && $(BACKEND_BIN) "$$POLYGON_API_KEY" "$$DB_PASSWORD" ) & \
	  sleep 1; \
	  ( cd bridge       && node bridge.js ) & \
	  ( cd frontend-react && npm run dev ) & \
	  wait