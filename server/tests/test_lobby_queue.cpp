#include <catch2/catch_test_macros.hpp>

#include "server/lobby_queue.h"

using namespace anjeer::server;

// broadcast_positions is not exercised here — it requires a live uWS::Loop.
// It is covered by the T10 WS integration tests.

TEST_CASE("LobbyQueue: first enqueue returns position 1", "[lobby_queue]") {
    LobbyQueue q(4);
    REQUIRE(q.enqueue("p1", "alice", nullptr) == 1);
}

TEST_CASE("LobbyQueue: enqueue at max_size returns -1", "[lobby_queue]") {
    LobbyQueue q(2);
    REQUIRE(q.enqueue("p1", "alice", nullptr) == 1);
    REQUIRE(q.enqueue("p2", "bob",   nullptr) == 2);
    REQUIRE(q.enqueue("p3", "carol", nullptr) == -1);
    REQUIRE(q.size() == 2);
}

TEST_CASE("LobbyQueue: dequeue recalculates positions", "[lobby_queue]") {
    LobbyQueue q(4);
    q.enqueue("p1", "alice", nullptr);
    q.enqueue("p2", "bob",   nullptr);
    q.enqueue("p3", "carol", nullptr);

    q.dequeue("p1");

    // p2 was position 2, now should be position 1.
    REQUIRE(q.position_of("p2") == 1);
    REQUIRE(q.position_of("p3") == 2);
    REQUIRE(q.size() == 2);
}

TEST_CASE("LobbyQueue: drain returns front N and removes them", "[lobby_queue]") {
    LobbyQueue q(4);
    q.enqueue("p1", "alice", nullptr);
    q.enqueue("p2", "bob",   nullptr);
    q.enqueue("p3", "carol", nullptr);

    const auto out = q.drain(2);

    REQUIRE(out.size() == 2);
    REQUIRE(out[0].player_id == "p1");
    REQUIRE(out[1].player_id == "p2");
    REQUIRE(q.size() == 1);
    REQUIRE(q.position_of("p3") == 1);
    REQUIRE_FALSE(q.has("p1"));
    REQUIRE_FALSE(q.has("p2"));
}

TEST_CASE("LobbyQueue: position_of returns 0 for absent player", "[lobby_queue]") {
    LobbyQueue q(4);
    q.enqueue("p1", "alice", nullptr);
    REQUIRE(q.position_of("unknown") == 0);
}

TEST_CASE("LobbyQueue: double enqueue returns -1", "[lobby_queue]") {
    LobbyQueue q(4);
    REQUIRE(q.enqueue("p1", "alice", nullptr) == 1);
    REQUIRE(q.enqueue("p1", "alice", nullptr) == -1);
    REQUIRE(q.size() == 1);
}
