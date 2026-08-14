cmake_minimum_required(VERSION 3.20)

add_library(anjeer_exchange STATIC
    src/order_book.cpp
    src/sequencer.cpp
    src/exchange_session.cpp
)

target_include_directories(anjeer_exchange PUBLIC include)
target_compile_features(anjeer_exchange PUBLIC cxx_std_17)

# ---------------------------------------------------------------------------
# OrderBook unit tests (Slice 13 Task 3 — moved from engine/tests/)
# AGENT-CTX: exchange_tests links only anjeer_exchange and Catch2 — no server,
# no uWS, no nlohmann, no engine. The exchange module must be testable in
# isolation. These tests were previously engine_tests; namespace updated from
# anjeer::engine to anjeer::exchange when OrderBook migrated to this module.
# ---------------------------------------------------------------------------
add_executable(exchange_tests
    tests/test_order_book.cpp
    tests/test_sequencer.cpp
    tests/test_exchange_session.cpp
)

target_link_libraries(exchange_tests PRIVATE
    anjeer_exchange
    Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(exchange_tests)

add_executable(exchange_bench EXCLUDE_FROM_ALL
    bench/bench_order_book.cpp
    src/order_book.cpp
)

target_include_directories(exchange_bench PRIVATE include)
target_compile_features(exchange_bench PRIVATE cxx_std_17)
target_compile_options(exchange_bench PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O2>
)
target_compile_definitions(exchange_bench PRIVATE NDEBUG)
