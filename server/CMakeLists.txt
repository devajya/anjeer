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
    src/session_repo.cpp
    src/keybinds_repo.cpp
    src/oauth_provider.cpp
    src/auth_service.cpp
    src/http_server.cpp
    src/event_bus.cpp
    src/crypto_util.cpp
    src/api_key_repo.cpp
    src/spectate_token_repo.cpp
    src/rate_limiter.cpp
    src/bot_scheduler.cpp
    src/bot_adapter.cpp
    src/bot_manager.cpp
    src/reconnect_token_repo.cpp
    src/game_slots_repo.cpp
    src/lobby_queue.cpp
    src/bot_manager_session.cpp
    src/eval_runner.cpp
    src/bayesian_eval_module.cpp
    src/accumulation_eval_module.cpp
    src/execution_eval_module.cpp
)

target_include_directories(server PRIVATE include)

target_link_libraries(server PRIVATE
    engine
    anjeer_exchange
    uWebSockets
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    CURL::libcurl
    Crow::Crow
    jwt-cpp::jwt-cpp
    readerwriterqueue
    crypto
    pthread
)

# ---------------------------------------------------------------------------
# EvalRunner unit tests — no DB, no network; pure thread-pool + queue logic
# ---------------------------------------------------------------------------
add_executable(eval_runner_tests
    tests/eval_runner_tests.cpp
    src/eval_runner.cpp
    src/bayesian_eval_module.cpp
    src/accumulation_eval_module.cpp
    src/execution_eval_module.cpp
)

target_include_directories(eval_runner_tests PRIVATE include)

target_link_libraries(eval_runner_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    nlohmann_json::nlohmann_json
    readerwriterqueue
    pthread
)

catch_discover_tests(eval_runner_tests)

# ---------------------------------------------------------------------------
# Bayesian eval module unit tests — no DB, no network; pure math + latency
# ---------------------------------------------------------------------------
add_executable(bayesian_module_tests
    tests/bayesian_module_tests.cpp
    src/bayesian_eval_module.cpp
    src/accumulation_eval_module.cpp
)

target_include_directories(bayesian_module_tests PRIVATE include)

target_link_libraries(bayesian_module_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    nlohmann_json::nlohmann_json
    pthread
)

catch_discover_tests(bayesian_module_tests)

# ---------------------------------------------------------------------------
# Accumulation eval module unit tests — no DB, no network; pure math + latency
# ---------------------------------------------------------------------------
add_executable(accumulation_module_tests
    tests/accumulation_module_tests.cpp
    src/accumulation_eval_module.cpp
)

target_include_directories(accumulation_module_tests PRIVATE include)

target_link_libraries(accumulation_module_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    nlohmann_json::nlohmann_json
    pthread
)

catch_discover_tests(accumulation_module_tests)

# ---------------------------------------------------------------------------
# Execution eval module unit tests — no DB, no network; pure math + latency
# ---------------------------------------------------------------------------
add_executable(execution_module_tests
    tests/execution_module_tests.cpp
    src/execution_eval_module.cpp
)

target_include_directories(execution_module_tests PRIVATE include)

target_link_libraries(execution_module_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    nlohmann_json::nlohmann_json
    pthread
)

catch_discover_tests(execution_module_tests)

# ---------------------------------------------------------------------------
# Eval integration tests — headless round simulation; all three modules + runner
# ---------------------------------------------------------------------------
add_executable(eval_integration_tests
    tests/eval_integration_tests.cpp
    src/eval_runner.cpp
    src/bayesian_eval_module.cpp
    src/accumulation_eval_module.cpp
    src/execution_eval_module.cpp
)

target_include_directories(eval_integration_tests PRIVATE include)

target_link_libraries(eval_integration_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    nlohmann_json::nlohmann_json
    readerwriterqueue
    pthread
)

catch_discover_tests(eval_integration_tests)

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
    src/eval_runner.cpp
    src/bayesian_eval_module.cpp
    src/accumulation_eval_module.cpp
    src/execution_eval_module.cpp
    src/game_session_wire.cpp
    src/session_repo.cpp
    src/logger.cpp
    src/db.cpp
    src/player_repo.cpp
    src/lobby_repo.cpp
    src/auth_service.cpp
    src/event_bus.cpp
    src/crypto_util.cpp
    src/api_key_repo.cpp
    src/rate_limiter.cpp
    src/bot_scheduler.cpp
    src/bot_adapter.cpp
    src/bot_manager.cpp
    src/bot_manager_session.cpp
    src/reconnect_token_repo.cpp
    src/game_slots_repo.cpp
    src/lobby_queue.cpp
)

target_include_directories(ws_server_tests PRIVATE include)

target_compile_definitions(ws_server_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(ws_server_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    anjeer_exchange
    uWebSockets
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    jwt-cpp::jwt-cpp
    readerwriterqueue
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
    src/keybinds_repo.cpp
    src/event_bus.cpp
    src/logger.cpp
    src/crypto_util.cpp
    src/api_key_repo.cpp
    src/spectate_token_repo.cpp
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

# ---------------------------------------------------------------------------
# SessionRepo integration tests
# ---------------------------------------------------------------------------
add_executable(session_repo_tests
    tests/test_session_repo.cpp
    src/db.cpp
    src/lobby_repo.cpp
    src/session_repo.cpp
)

target_include_directories(session_repo_tests PRIVATE include)

target_compile_definitions(session_repo_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(session_repo_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    crypto
)

add_test(NAME session_repo_tests COMMAND session_repo_tests)
set_tests_properties(session_repo_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# Lobby resilience tests (Slice 7 acceptance criteria L1-L5)
# ---------------------------------------------------------------------------
add_executable(lobby_resilience_tests
    tests/lobby_resilience_tests.cpp
    src/db.cpp
    src/lobby_repo.cpp
)

target_include_directories(lobby_resilience_tests PRIVATE include)

target_compile_definitions(lobby_resilience_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(lobby_resilience_tests PRIVATE
    Catch2::Catch2WithMain
    libpqxx::pqxx
    crypto
)

add_test(NAME lobby_resilience_tests COMMAND lobby_resilience_tests)
set_tests_properties(lobby_resilience_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# GameSession unit tests (Slice 7 acceptance criteria G1-G10)
# ---------------------------------------------------------------------------
add_executable(game_session_tests
    tests/game_session_tests.cpp
    src/config.cpp
    src/db.cpp
    src/lobby_repo.cpp
    src/session_repo.cpp
    src/game_session.cpp
    src/eval_runner.cpp
    src/bayesian_eval_module.cpp
    src/accumulation_eval_module.cpp
    src/execution_eval_module.cpp
    src/game_session_wire.cpp
    src/logger.cpp
    src/event_bus.cpp
)

target_include_directories(game_session_tests PRIVATE include)

target_compile_definitions(game_session_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(game_session_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    anjeer_exchange
    uWebSockets
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    readerwriterqueue
    crypto
    pthread
)

add_test(NAME game_session_tests COMMAND game_session_tests)
set_tests_properties(game_session_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# session_queue_tsan_tests — lock-free SPSC queue under concurrent use (T1)
# ---------------------------------------------------------------------------
# AGENT-CTX: Minimal deps on purpose — this test only exercises the queue, not
# the server stack. Keeping it lean means it also compiles quickly in CI when
# only the tsan preset is needed. No DB, no uWS, no nlohmann.
add_executable(session_queue_tsan_tests
    tests/session_queue_tsan_tests.cpp
)

target_include_directories(session_queue_tsan_tests PRIVATE include)

target_link_libraries(session_queue_tsan_tests PRIVATE
    Catch2::Catch2WithMain
    readerwriterqueue
    pthread
)

add_test(NAME session_queue_tsan_tests COMMAND session_queue_tsan_tests)
# AGENT-CTX: RUN_SERIAL prevents other threaded tests from running in parallel
# with this one. Under ThreadSanitizer, concurrent test processes sharing the
# same shadow-memory region can produce spurious cross-test race reports.
set_tests_properties(session_queue_tsan_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# API key repo + crypto_util integration tests
# ---------------------------------------------------------------------------
add_executable(api_key_repo_tests
    tests/test_api_key_repo.cpp
    src/db.cpp
    src/crypto_util.cpp
    src/api_key_repo.cpp
)

target_include_directories(api_key_repo_tests PRIVATE include)

target_compile_definitions(api_key_repo_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(api_key_repo_tests PRIVATE
    Catch2::Catch2WithMain
    libpqxx::pqxx
    crypto
)

add_test(NAME api_key_repo_tests COMMAND api_key_repo_tests)
set_tests_properties(api_key_repo_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# Rate limiter unit tests — no DB, no network, pure in-process logic
# ---------------------------------------------------------------------------
add_executable(rate_limiter_tests
    tests/test_rate_limiter.cpp
    src/rate_limiter.cpp
)

target_include_directories(rate_limiter_tests PRIVATE include)

target_link_libraries(rate_limiter_tests PRIVATE
    Catch2::Catch2WithMain
)

catch_discover_tests(rate_limiter_tests)

# ---------------------------------------------------------------------------
# FeedTier unit tests — header-only enum + string helpers, no DB, no network
# ---------------------------------------------------------------------------
add_executable(feed_tier_tests
    tests/test_feed_tier.cpp
)

target_include_directories(feed_tier_tests PRIVATE include)

target_link_libraries(feed_tier_tests PRIVATE
    Catch2::Catch2WithMain
)

catch_discover_tests(feed_tier_tests)

# ---------------------------------------------------------------------------
# BotScheduler unit tests — no DB, no network, pure thread-pool logic
# ---------------------------------------------------------------------------
add_executable(bot_scheduler_tests
    tests/test_bot_scheduler.cpp
    src/bot_scheduler.cpp
)

target_include_directories(bot_scheduler_tests PRIVATE include)

target_link_libraries(bot_scheduler_tests PRIVATE
    Catch2::Catch2WithMain
    pthread
)

catch_discover_tests(bot_scheduler_tests)

# ---------------------------------------------------------------------------
# BotAdapter unit tests — no DB, no network; exercises SPSC queue flow
# ---------------------------------------------------------------------------
add_executable(bot_adapter_tests
    tests/test_bot_adapter.cpp
    src/bot_adapter.cpp
    src/bot_scheduler.cpp
)

target_include_directories(bot_adapter_tests PRIVATE include)

target_link_libraries(bot_adapter_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    anjeer_exchange
    nlohmann_json::nlohmann_json
    readerwriterqueue
    pthread
)

catch_discover_tests(bot_adapter_tests)

# ---------------------------------------------------------------------------
# BotManager unit tests — no DB, pure in-memory state
# ---------------------------------------------------------------------------
add_executable(bot_manager_tests
    tests/test_bot_manager.cpp
    src/bot_manager.cpp
    src/bot_scheduler.cpp
    src/bot_adapter.cpp
)

target_include_directories(bot_manager_tests PRIVATE include)

target_link_libraries(bot_manager_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    anjeer_exchange
    nlohmann_json::nlohmann_json
    readerwriterqueue
    pthread
)

catch_discover_tests(bot_manager_tests)

# ---------------------------------------------------------------------------
# Bot headless integration tests — full game with bot-only sessions; needs DB
# ---------------------------------------------------------------------------
add_executable(bot_headless_tests
    tests/test_bot_headless.cpp
    src/config.cpp
    src/db.cpp
    src/game_session.cpp
    src/eval_runner.cpp
    src/bayesian_eval_module.cpp
    src/accumulation_eval_module.cpp
    src/game_session_wire.cpp
    src/session_repo.cpp
    src/lobby_repo.cpp
    src/logger.cpp
    src/event_bus.cpp
    src/bot_scheduler.cpp
    src/bot_adapter.cpp
    src/bot_manager.cpp
    src/bot_manager_session.cpp
)

target_include_directories(bot_headless_tests PRIVATE include)

target_compile_definitions(bot_headless_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(bot_headless_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    anjeer_exchange
    uWebSockets
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    readerwriterqueue
    crypto
    pthread
)

add_test(NAME bot_headless_tests COMMAND bot_headless_tests)
set_tests_properties(bot_headless_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# ReconnectTokenRepo integration tests — requires anjeer_test DB
# ---------------------------------------------------------------------------
add_executable(reconnect_token_repo_tests
    tests/test_reconnect_token_repo.cpp
    src/db.cpp
    src/crypto_util.cpp
    src/reconnect_token_repo.cpp
)

target_include_directories(reconnect_token_repo_tests PRIVATE include)

target_compile_definitions(reconnect_token_repo_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(reconnect_token_repo_tests PRIVATE
    Catch2::Catch2WithMain
    libpqxx::pqxx
    crypto
)

add_test(NAME reconnect_token_repo_tests COMMAND reconnect_token_repo_tests)
set_tests_properties(reconnect_token_repo_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# SimpleCache unit tests — header-only template, no DB, no network
# ---------------------------------------------------------------------------
add_executable(simple_cache_tests
    tests/test_simple_cache.cpp
)

target_include_directories(simple_cache_tests PRIVATE include)

target_link_libraries(simple_cache_tests PRIVATE
    Catch2::Catch2WithMain
)

catch_discover_tests(simple_cache_tests)

# ---------------------------------------------------------------------------
# GameSlotsRepo integration tests — requires anjeer_test DB
# ---------------------------------------------------------------------------
add_executable(game_slots_repo_tests
    tests/test_game_slots_repo.cpp
    src/db.cpp
    src/game_slots_repo.cpp
)

target_include_directories(game_slots_repo_tests PRIVATE include)

target_compile_definitions(game_slots_repo_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(game_slots_repo_tests PRIVATE
    Catch2::Catch2WithMain
    libpqxx::pqxx
    pthread
)

add_test(NAME game_slots_repo_tests COMMAND game_slots_repo_tests)
set_tests_properties(game_slots_repo_tests PROPERTIES RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# LobbyQueue unit tests — no DB, no network; pure in-process queue logic
# ---------------------------------------------------------------------------
# AGENT-CTX: lobby_queue.h includes ws_types.h which pulls in uWS App.h, so
# the test binary must link uWebSockets and crypto (OpenSSL) even though no
# live server is started. broadcast_positions is NOT tested here; it requires
# a live uWS::Loop and is covered by T10 WS integration tests.
add_executable(lobby_queue_tests
    tests/test_lobby_queue.cpp
    src/lobby_queue.cpp
)

target_include_directories(lobby_queue_tests PRIVATE include)

target_link_libraries(lobby_queue_tests PRIVATE
    Catch2::Catch2WithMain
    uWebSockets
    nlohmann_json::nlohmann_json
    crypto
    pthread
)

catch_discover_tests(lobby_queue_tests)

# ---------------------------------------------------------------------------
# Reconnect + queue integration tests — full WsServer, requires anjeer_test DB
# ---------------------------------------------------------------------------
add_executable(reconnect_integration_tests
    tests/test_reconnect_integration.cpp
    src/config.cpp
    src/ws_server.cpp
    src/lobby_gateway.cpp
    src/game_session.cpp
    src/eval_runner.cpp
    src/bayesian_eval_module.cpp
    src/accumulation_eval_module.cpp
    src/execution_eval_module.cpp
    src/game_session_wire.cpp
    src/session_repo.cpp
    src/logger.cpp
    src/db.cpp
    src/player_repo.cpp
    src/lobby_repo.cpp
    src/auth_service.cpp
    src/event_bus.cpp
    src/crypto_util.cpp
    src/api_key_repo.cpp
    src/rate_limiter.cpp
    src/bot_scheduler.cpp
    src/bot_adapter.cpp
    src/bot_manager.cpp
    src/bot_manager_session.cpp
    src/reconnect_token_repo.cpp
    src/game_slots_repo.cpp
    src/lobby_queue.cpp
)

target_include_directories(reconnect_integration_tests PRIVATE include)

target_compile_definitions(reconnect_integration_tests PRIVATE
    TEST_DB_CONN="postgresql:///anjeer_test"
    TEST_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/db/migrations"
)

target_link_libraries(reconnect_integration_tests PRIVATE
    Catch2::Catch2WithMain
    engine
    anjeer_exchange
    uWebSockets
    nlohmann_json::nlohmann_json
    libpqxx::pqxx
    jwt-cpp::jwt-cpp
    readerwriterqueue
    crypto
    pthread
)

add_test(NAME reconnect_integration_tests COMMAND reconnect_integration_tests)
set_tests_properties(reconnect_integration_tests PROPERTIES RUN_SERIAL TRUE)
