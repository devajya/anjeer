#include <catch2/catch_test_macros.hpp>
#include "exchange/sequencer.h"

using namespace anjeer::exchange;

TEST_CASE("Sequencer increments monotonically") {
    Sequencer seq;
    REQUIRE(seq.next_seq() == 1);
    REQUIRE(seq.next_seq() == 2);
    REQUIRE(seq.next_seq() == 3);
}

TEST_CASE("Sequencer reset restarts from 1") {
    Sequencer seq;
    seq.next_seq();
    seq.next_seq();
    seq.reset();
    REQUIRE(seq.next_seq() == 1);
}

TEST_CASE("Sequencer reset on fresh instance still returns 1") {
    Sequencer seq;
    seq.reset();
    REQUIRE(seq.next_seq() == 1);
}

TEST_CASE("Sequencer multiple resets are idempotent") {
    Sequencer seq;
    seq.next_seq(); // 1
    seq.next_seq(); // 2
    seq.reset();
    seq.reset();
    REQUIRE(seq.next_seq() == 1);
    REQUIRE(seq.next_seq() == 2);
}

// T01 — current_seq() returns 0 before first call, N after N calls, 0 after reset()
TEST_CASE("T01 Sequencer::current_seq returns 0 before first next_seq call") {
    Sequencer seq;
    REQUIRE(seq.current_seq() == 0);
}

TEST_CASE("T01 Sequencer::current_seq tracks last issued seq") {
    Sequencer seq;
    seq.next_seq(); // 1
    REQUIRE(seq.current_seq() == 1);
    seq.next_seq(); // 2
    seq.next_seq(); // 3
    REQUIRE(seq.current_seq() == 3);
}

TEST_CASE("T01 Sequencer::current_seq returns 0 after reset") {
    Sequencer seq;
    seq.next_seq();
    seq.next_seq();
    seq.reset();
    REQUIRE(seq.current_seq() == 0);
}
