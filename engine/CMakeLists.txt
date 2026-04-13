cmake_minimum_required(VERSION 3.20)

# AGENT-CTX: engine is a static library that grows slice by slice.
# Never add network or I/O code here — that belongs in server/.
# Slice 1 — stub only.
# Slice 2 — OrderBook: price-time priority matching, nudge, cancel.
add_library(engine STATIC
    src/order_book.cpp
)

target_include_directories(engine PUBLIC include)
target_compile_features(engine PUBLIC cxx_std_17)

# ---------------------------------------------------------------------------
# Unit tests (Catch2)
# AGENT-CTX: engine_tests links only against engine and Catch2 — no server,
# no uWS, no nlohmann. The engine must be testable in isolation.
# If a test ever needs to link server code, it belongs in server/tests/ instead.
# TEST_FIXTURES_DIR is not needed here (engine tests have no file I/O).
# ---------------------------------------------------------------------------
add_executable(engine_tests
    tests/test_order_book.cpp
)

target_link_libraries(engine_tests PRIVATE
    engine
    Catch2::Catch2WithMain
)

# AGENT-CTX: include(CTest) + include(Catch) follow the same pattern as
# server/CMakeLists.txt. include(Catch) provides catch_discover_tests(), which
# registers each TEST_CASE as a separate CTest entry so ctest --output-on-failure
# reports individual test names, not just a binary pass/fail.
# The root CMakeLists already calls enable_testing(); these includes extend it.
include(CTest)
include(Catch)
catch_discover_tests(engine_tests)
