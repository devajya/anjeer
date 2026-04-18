cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------
add_executable(server
    src/main.cpp
    src/config.cpp
    src/ws_server.cpp
    src/game_session.cpp
    src/game_session_wire.cpp
    src/logger.cpp
    src/db.cpp
    src/player_repo.cpp
    src/oauth_provider.cpp
    src/auth_service.cpp
    src/http_server.cpp
)

target_include_directories(server PRIVATE include)

# AGENT-CTX: pthread is required by uWebSockets/uSockets on Linux.
# nlohmann_json::nlohmann_json is header-only; linking pulls in include paths only.
# libpqxx::pqxx — C++ PostgreSQL client (wraps system libpq); added in Slice 5.
# CURL::libcurl — HTTP client for OAuth code exchange; added in Slice 5.
# Crow::Crow — header-only async HTTP/REST server; added in Slice 5.
# jwt-cpp::jwt-cpp — header-only JWT signing/validation; added in Slice 5.
# stdc++fs is not linked explicitly: GCC 9+ and Clang 9+ include <filesystem>
# in the standard library. If building on an older toolchain, add stdc++fs here.
target_link_libraries(server PRIVATE
    engine
    uWebSockets
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    CURL::libcurl
    Crow::Crow
    jwt-cpp::jwt-cpp
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
    src/game_session.cpp
    src/game_session_wire.cpp
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

# ---------------------------------------------------------------------------
# Auth integration tests — require a live anjeer_test PostgreSQL database
# ---------------------------------------------------------------------------
# AGENT-CTX: auth_integration_tests links db.cpp + player_repo.cpp directly
# (not via a shared library) consistent with the pattern used by server_tests
# (compiles config.cpp directly). A server_lib target could unify this in a
# future slice if the list of shared TUs grows large.
# TEST_DB_CONN is injected at compile time so the binary does not need a config
# file. Run `createdb anjeer_test && make db-migrate DB_CONN=postgresql://localhost:5432/anjeer_test`
# before running these tests.
# RUN_SERIAL: integration tests touch the DB; running in parallel risks table
# truncation interleaving with reads from another test instance.
add_executable(auth_integration_tests
    tests/test_auth_integration.cpp
    src/config.cpp
    src/db.cpp
    src/player_repo.cpp
    src/oauth_provider.cpp
    src/auth_service.cpp
    # AGENT-CTX: oauth_provider.cpp and auth_service.cpp are compiled directly
    # into the test binary (not via a shared lib) consistent with the existing
    # pattern. If the list of shared TUs grows unwieldy, extract a server_lib
    # target in a future slice.
)

target_include_directories(auth_integration_tests PRIVATE include)

target_compile_definitions(auth_integration_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    # AGENT-CTX: TEST_MIGRATIONS_DIR uses CMAKE_SOURCE_DIR (project root) not
    # CMAKE_CURRENT_SOURCE_DIR (server/) because db/migrations lives at the top
    # level. Tests run from the build directory, so a relative path would fail.
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(auth_integration_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    jwt-cpp::jwt-cpp
    CURL::libcurl
)

add_test(NAME auth_integration_tests COMMAND auth_integration_tests)
set_tests_properties(auth_integration_tests PROPERTIES RUN_SERIAL TRUE)
