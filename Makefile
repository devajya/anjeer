BUILD_DIR := build
CONFIG    := config/default.json

.PHONY: build build-engine build-server build-frontend \
        dev dev-server dev-frontend \
        test test-unit test-frontend \
        clean clean-all fmt

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

# AGENT-CTX: Parallel dev runner using shell & and wait with a trap for clean exit.
# Both processes are started in the background; trap kills both on Ctrl+C.
# No external tools (concurrently, foreman) — keeps the dev dependency footprint minimal.
# Limitation: build output from both processes interleaves in the terminal.
dev:
	@trap 'kill %1 %2 2>/dev/null; exit 0' INT TERM; \
	$(MAKE) dev-server & \
	$(MAKE) dev-frontend & \
	wait

# ---------------------------------------------------------------------------
# Test targets
# ---------------------------------------------------------------------------
test-unit: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --target server_tests --parallel
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

# AGENT-CTX: fmt runs clang-format in-place on all C++ source and header files,
# and prettier on frontend TypeScript/CSS. The `|| true` prevents a non-zero exit
# if prettier is not installed (optional in early slices).
fmt:
	find engine server -name '*.cpp' -o -name '*.h' | xargs clang-format -i
	npm run format --prefix frontend 2>/dev/null || true
