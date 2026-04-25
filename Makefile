# Top-level Makefile — builds every component of stock-app.
#
# Usage:
#   make              # build backend + install bridge/bot deps + build frontend
#   make backend      # C backend only
#   make frontend     # React production bundle (frontend-react/dist)
#   make bridge       # install Node bridge deps
#   make bots         # install Python bot deps
#   make clean        # remove all build artifacts
#   make run          # launch backend + bridge + frontend dev server together
#                     # (requires .env with POLYGON_API_KEY; Ctrl-C stops all three)

.PHONY: all backend frontend bridge bots clean run

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

clean:
	$(MAKE) -C backend clean
	rm -rf frontend-react/dist frontend-react/node_modules
	rm -rf bridge/node_modules
	@echo "[clean] done (Python venv left intact — remove bots/venv manually if desired)"

# Launch all three long-running processes. Backend reads $POLYGON_API_KEY from
# the environment (sourced from .env). Ctrl-C traps SIGINT and kills all three.
run:
	@test -f .env || (echo "ERROR: .env not found. Create one and fill in POLYGON_API_KEY."; exit 1)
	@set -a; . ./.env; set +a; \
	  test -n "$$POLYGON_API_KEY" || (echo "ERROR: POLYGON_API_KEY is empty in .env"; exit 1); \
	  trap 'kill 0' INT TERM; \
	  ( cd backend      && ./stock-backend "$$POLYGON_API_KEY" ) & \
	  sleep 1; \
	  ( cd bridge       && node bridge.js ) & \
	  ( cd frontend-react && npm run dev ) & \
	  wait