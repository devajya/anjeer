BUILD_DIR := build
CONFIG    := config/default.json
# AGENT-CTX: DB_CONN is used by db-migrate for manual/dev migration runs.
# The server also runs migrations automatically on startup via DbMigrator::run().
# Override on the command line: make db-migrate DB_CONN=postgresql://...
# AGENT-CTX: No host in the URI = Unix domain socket = peer auth (no password).
# TCP (postgresql://localhost/...) triggers md5/password auth on Ubuntu by default.
# In production (Slice 16), the RDS connection string will include host+credentials
# sourced from AWS SSM — override DB_CONN on the command line for that case.
DB_CONN   ?= postgresql:///anjeer_dev

.PHONY: build build-engine build-server build-frontend \
        dev dev-server dev-frontend \
        test test-unit test-frontend \
        clean clean-all fmt install-hooks install-deps \
        db-migrate db-seed

# ---------------------------------------------------------------------------
# CMake configure
# AGENT-CTX: Sentinel is build/Makefile, NOT build/CMakeCache.txt.
# CMakeCache.txt is written early in configure even on a partial/failed run,
# so using it as the target causes make to skip re-running cmake when the
# build Makefile is missing. build/Makefile only exists after a fully successful
# configure+generate step — it is the reliable readiness indicator.
#
# CMakeLists.txt files are listed as prerequisites so cmake re-runs automatically
# when any CMakeLists changes, even if build/Makefile already exists.
#
# FetchContent deps live in .deps/ (set via FETCHCONTENT_BASE_DIR in CMakeLists.txt)
# so they survive `make clean` and are never re-downloaded unnecessarily.
# ---------------------------------------------------------------------------
$(BUILD_DIR)/Makefile: CMakeLists.txt engine/CMakeLists.txt server/CMakeLists.txt
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug

# ---------------------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------------------
build-engine: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --target engine --parallel

build-server: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --target server --parallel

build-frontend:
	npm run build --prefix frontend

build: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --parallel
	npm run build --prefix frontend

# ---------------------------------------------------------------------------
# Dev run targets
# ---------------------------------------------------------------------------
dev-server: build-server
	./$(BUILD_DIR)/server/server --config $(CONFIG)

dev-frontend:
	npm run dev --prefix frontend

# AGENT-CTX: Parallel dev runner. Builds the server binary first (blocking), then
# starts the server and Vite in background jobs whose PIDs are captured directly.
# The trap kills those exact PIDs on Ctrl+C, guaranteeing the server binary is
# cleaned up and never left as a stale SO_REUSEPORT orphan on port 9001.
#
# Previous design used `$(MAKE) dev-server &` which spawned a child make process;
# kill %1 killed that child make but NOT its grandchild (the server binary itself).
# Multiple `make dev` invocations stacked up 8+ server processes competing on 9001
# via SO_REUSEPORT — connections went to old (pre-logging) binaries, making the
# new binary look like it received no traffic.
#
# Also kills any pre-existing server binary on port 9001 before starting, so a
# manually-interrupted previous run never causes the same problem.
dev: build-server
	@fuser -k 9001/tcp 2>/dev/null || true; \
	trap 'kill $$server_pid $$vite_pid 2>/dev/null; exit 0' INT TERM; \
	./$(BUILD_DIR)/server/server --config $(CONFIG) & server_pid=$$!; \
	npm run dev --prefix frontend & vite_pid=$$!; \
	wait

# ---------------------------------------------------------------------------
# Test targets
# ---------------------------------------------------------------------------
test-unit: $(BUILD_DIR)/Makefile
	# AGENT-CTX: Build all test binaries explicitly before running ctest.
	# Adding a new test binary in a subdirectory CMakeLists requires a matching
	# --target line here. ctest discovers all registered tests from all binaries.
	cmake --build $(BUILD_DIR) --target engine_tests --parallel
	cmake --build $(BUILD_DIR) --target game_state_tests --parallel
	cmake --build $(BUILD_DIR) --target scoring_engine_tests --parallel
	cmake --build $(BUILD_DIR) --target server_tests --parallel
	cmake --build $(BUILD_DIR) --target ws_server_tests --parallel
	cmake --build $(BUILD_DIR) --target auth_integration_tests --parallel
	cd $(BUILD_DIR) && ctest --output-on-failure

test-frontend:
	npm test --prefix frontend

test: test-unit test-frontend

# ---------------------------------------------------------------------------
# Utility
# ---------------------------------------------------------------------------

# AGENT-CTX: `clean` wipes compiled build artifacts but preserves:
#   - .deps/  (FetchContent sources — re-downloading wastes minutes)
#   - frontend/node_modules (npm install takes time)
# Use this after code changes to force a full recompile.
# Use `clean-all` when you need a true from-scratch rebuild (e.g. dep version bump).
clean:
	rm -rf $(BUILD_DIR)
	rm -rf frontend/dist

# Full reset: removes everything including downloaded deps and node_modules.
# Re-running make after this will re-download all FetchContent deps and npm packages.
clean-all:
	rm -rf $(BUILD_DIR)
	rm -rf .deps
	rm -rf frontend/node_modules frontend/dist

# Installs the doc-staleness pre-commit hook. Run once after cloning.
# The hook warns (but does not block) when a new engine/server header or
# frontend component is staged without updating CLAUDE.md or ARCHITECTURE.md.
install-hooks:
	@printf '#!/usr/bin/env bash\nbash scripts/check-secrets.sh || exit 1\nbash scripts/check-docs-hook.sh\n' > .git/hooks/pre-commit
	chmod +x .git/hooks/pre-commit
	@echo "pre-commit hook installed (secrets + docs checks)"

# AGENT-CTX: System packages required by Slice 5+ deps that are NOT FetchContent.
# libpq-dev   — PostgreSQL C client headers/lib; libpqxx wraps this at compile+link time.
# libcurl4-openssl-dev — HTTP client used by OAuth provider implementations.
# Both must be present before `cmake -B build` runs (find_package checks at configure time).
# This target is Ubuntu/Debian only. On other distros install the equivalent packages.
install-deps:
	sudo apt-get update -qq
	sudo apt-get install -y libpq-dev libcurl4-openssl-dev

# AGENT-CTX: db-migrate applies all pending SQL files in db/migrations/ by
# bootstrapping schema_migrations and running each file via psql in version order.
# This is a dev convenience — the server also runs DbMigrator::run() automatically
# on startup. Use this target to inspect schema state without starting the server.
# For the test DB, run: make db-migrate DB_CONN=postgresql://localhost:5432/anjeer_test
db-migrate:
	@psql "$(DB_CONN)" -v ON_ERROR_STOP=1 \
	    -c "CREATE TABLE IF NOT EXISTS schema_migrations (version INT PRIMARY KEY, applied_at TIMESTAMPTZ NOT NULL DEFAULT now());"
	@for f in $$(ls db/migrations/*.sql | sort); do \
	    ver=$$(basename $$f | sed 's/_.*//' | sed 's/^0*//'); \
	    exists=$$(psql "$(DB_CONN)" -tAc "SELECT 1 FROM schema_migrations WHERE version=$$ver"); \
	    if [ "$$exists" != "1" ]; then \
	        echo "Applying $$f (version $$ver)..."; \
	        psql "$(DB_CONN)" -v ON_ERROR_STOP=1 -f "$$f"; \
	        psql "$(DB_CONN)" -c "INSERT INTO schema_migrations (version) VALUES ($$ver);"; \
	    else \
	        echo "Skipping $$f (already applied)"; \
	    fi; \
	done
	@echo "Migrations complete."

db-seed:
	@psql "$(DB_CONN)" -v ON_ERROR_STOP=1 -f db/seeds/dev_users.sql
	@echo "Seed data inserted."

# AGENT-CTX: fmt runs clang-format in-place on all C++ source and header files,
# and prettier on frontend TypeScript/CSS. The `|| true` prevents a non-zero exit
# if prettier is not installed (optional in early slices).
fmt:
	find engine server -name '*.cpp' -o -name '*.h' | xargs clang-format -i
	npm run format --prefix frontend 2>/dev/null || true
