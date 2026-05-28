cmake_minimum_required(VERSION 3.20)

# AGENT-CTX: engine is a static library that grows slice by slice.
# Never add network or I/O code here — that belongs in server/.
# Slice 1 — stub only.
# Slice 2 — OrderBook: price-time priority matching, nudge, cancel.
# Slice 3 — GameState: deck composition, shuffle/deal, goal-suit derivation,
#            4× OrderBook composition, per-player hand tracking.
#            suit.h is header-only (constexpr); game_state.cpp has the impl.
add_library(engine STATIC
    src/game_state.cpp
    src/scoring_engine.cpp
    src/bots/bot_agent.cpp
    src/bots/easy_bot.cpp
    src/bots/medium_bot.cpp
    src/bots/hard_bot.cpp
)

target_include_directories(engine PUBLIC include)
target_compile_features(engine PUBLIC cxx_std_17)
target_link_libraries(engine PUBLIC anjeer_exchange)

# ---------------------------------------------------------------------------
# Unit tests (Catch2)
# AGENT-CTX: engine_tests was removed in Slice 13 Task 3 — its only source file
# (test_order_book.cpp) moved to exchange/tests/ when OrderBook migrated to the
# exchange module. engine_tests no longer exists; OrderBook tests run as
# exchange_tests. game_state_tests, scoring_engine_tests, and bot_tests remain
# here as separate binaries. Keeping them separate means a compilation error in
# one does not block running the others.
# ---------------------------------------------------------------------------

# AGENT-CTX: game_state_tests is a separate binary so it can be built and run
# independently. Both it and the other test binaries below link the same engine
# static library.
add_executable(game_state_tests
    tests/game_state_tests.cpp
)

target_link_libraries(game_state_tests PRIVATE
    engine
    Catch2::Catch2WithMain
)

# AGENT-CTX: include(CTest) + include(Catch) follow the same pattern as
# server/CMakeLists.txt. include(Catch) provides catch_discover_tests(), which
# registers each TEST_CASE as a separate CTest entry so ctest --output-on-failure
# reports individual test names, not just a binary pass/fail.
# The root CMakeLists already calls enable_testing(); these includes extend it.
add_executable(scoring_engine_tests
    tests/scoring_engine_tests.cpp
)

target_link_libraries(scoring_engine_tests PRIVATE
    engine
    Catch2::Catch2WithMain
)

add_executable(bot_tests
    tests/test_bots.cpp
)

target_link_libraries(bot_tests PRIVATE
    engine
    Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(game_state_tests)
catch_discover_tests(scoring_engine_tests)
catch_discover_tests(bot_tests)
