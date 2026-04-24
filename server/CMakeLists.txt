cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Server executable
# ---------------------------------------------------------------------------
add_executable(server
    src/main.cpp
    src/config.cpp
    src/ws_server.cpp
    src/lobby_gateway.cpp
    src/game_session.cpp
    src/game_session_wire.cpp
    src/logger.cpp
    src/db.cpp
    src/player_repo.cpp
    src/lobby_repo.cpp
    src/oauth_provider.cpp
    src/auth_service.cpp
    src/http_server.cpp
    src/event_bus.cpp
)

target_include_directories(server PRIVATE include)

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

add_executable(ws_server_tests
    tests/test_ws_server.cpp
    src/config.cpp
    src/ws_server.cpp
    src/lobby_gateway.cpp
    src/game_session.cpp
    src/game_session_wire.cpp
    src/logger.cpp
    src/db.cpp
    src/lobby_repo.cpp
    src/event_bus.cpp
)

target_include_directories(ws_server_tests PRIVATE include)

target_compile_definitions(ws_server_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(ws_server_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    uWebSockets
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    crypto
    pthread
)

# Register as a single test (not per-case) to avoid port conflicts when
# ctest runs multiple instances of the binary in parallel.
add_test(NAME ws_server_tests COMMAND ws_server_tests)
set_tests_properties(ws_server_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# Auth integration tests — require a live anjeer_test PostgreSQL database
# ---------------------------------------------------------------------------
add_executable(auth_integration_tests
    tests/test_auth_integration.cpp
    src/config.cpp
    src/db.cpp
    src/player_repo.cpp
    src/oauth_provider.cpp
    src/auth_service.cpp
)

target_include_directories(auth_integration_tests PRIVATE include)

target_compile_definitions(auth_integration_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
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

# ---------------------------------------------------------------------------
# EventBus unit tests — no DB, no network, no engine; pure in-process
# ---------------------------------------------------------------------------
add_executable(event_bus_tests
    tests/test_event_bus.cpp
    src/event_bus.cpp
)

target_include_directories(event_bus_tests PRIVATE include)

target_link_libraries(event_bus_tests PRIVATE
    Catch2::Catch2WithMain
)

catch_discover_tests(event_bus_tests)

# ---------------------------------------------------------------------------
# Lobby integration tests — require a live anjeer_test PostgreSQL database
# ---------------------------------------------------------------------------
add_executable(lobby_tests
    tests/test_lobby.cpp
    src/db.cpp
    src/lobby_repo.cpp
)

target_include_directories(lobby_tests PRIVATE include)

target_compile_definitions(lobby_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(lobby_tests PRIVATE
    Catch2::Catch2WithMain
    libpqxx::pqxx
    crypto
)

add_test(NAME lobby_tests COMMAND lobby_tests)
set_tests_properties(lobby_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# HTTP lobby integration tests — spin up HttpServer, call lobby REST endpoints
# ---------------------------------------------------------------------------
add_executable(http_lobby_tests
    tests/test_http_lobby.cpp
    src/config.cpp
    src/db.cpp
    src/player_repo.cpp
    src/lobby_repo.cpp
    src/oauth_provider.cpp
    src/auth_service.cpp
    src/http_server.cpp
    src/event_bus.cpp
    src/logger.cpp
)

target_include_directories(http_lobby_tests PRIVATE include)

target_compile_definitions(http_lobby_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(http_lobby_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    CURL::libcurl
    Crow::Crow
    jwt-cpp::jwt-cpp
    crypto
    pthread
)

add_test(NAME http_lobby_tests COMMAND http_lobby_tests)
set_tests_properties(http_lobby_tests PROPERTIES RUN_SERIAL TRUE)
