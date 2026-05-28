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
