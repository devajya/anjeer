cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------
add_executable(server
    src/main.cpp
    src/config.cpp
    src/ws_server.cpp
    src/logger.cpp
)

target_include_directories(server PRIVATE include)

# AGENT-CTX: pthread is required by uWebSockets/uSockets on Linux.
# nlohmann_json::nlohmann_json is header-only; linking pulls in include paths only.
target_link_libraries(server PRIVATE
    engine
    uWebSockets
    nlohmann_json::nlohmann_json
    pthread
)

# ---------------------------------------------------------------------------
# Unit tests (Catch2)
# ---------------------------------------------------------------------------

# AGENT-CTX: config.cpp is compiled directly into the test binary rather than
# linking a separate config library. This keeps the build graph simple for Slice 1.
# If more modules need testing, extract a server_lib target in a later slice.
add_executable(server_tests
    tests/test_config.cpp
    src/config.cpp
)

target_include_directories(server_tests PRIVATE include)

# Inject fixture path at compile time — avoids CTest working-directory issues.
target_compile_definitions(server_tests PRIVATE
    TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
)

target_link_libraries(server_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
)

include(CTest)
include(Catch)
catch_discover_tests(server_tests)

# ---------------------------------------------------------------------------
# Integration tests — spin up a real WsServer and connect a WS client
# ---------------------------------------------------------------------------
#
# AGENT-CTX: ws_server_tests links the full server stack (ws_server.cpp,
# config.cpp, logger.cpp) so the test covers the parse → validate → engine
# → serialise path end-to-end. Registered as a single CTest entry with
# RUN_SERIAL so it never races with itself if ctest is run with --parallel.

add_executable(ws_server_tests
    tests/test_ws_server.cpp
    src/config.cpp
    src/ws_server.cpp
    src/logger.cpp
)

target_include_directories(ws_server_tests PRIVATE include)

target_link_libraries(ws_server_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    uWebSockets
    nlohmann_json::nlohmann_json
    pthread
)

# Register as a single test (not per-case) to avoid port conflicts when
# ctest runs multiple instances of the binary in parallel.
add_test(NAME ws_server_tests COMMAND ws_server_tests)
set_tests_properties(ws_server_tests PROPERTIES RUN_SERIAL TRUE)
