#include <catch2/catch_test_macros.hpp>
#include "server/event_bus.h"

using anjeer::server::LocalEventBus;

TEST_CASE("LocalEventBus delivers to subscriber") {
    LocalEventBus bus;
    std::string received;
    bus.subscribe("ch", [&](const std::string& p) { received = p; });
    bus.publish("ch", "hello");
    REQUIRE(received == "hello");
}

TEST_CASE("LocalEventBus delivers to all subscribers on same channel") {
    LocalEventBus bus;
    int count = 0;
    bus.subscribe("ch", [&](const std::string&) { ++count; });
    bus.subscribe("ch", [&](const std::string&) { ++count; });
    bus.publish("ch", "x");
    REQUIRE(count == 2);
}

TEST_CASE("LocalEventBus does not deliver after unsubscribe") {
    LocalEventBus bus;
    int count = 0;
    auto id = bus.subscribe("ch", [&](const std::string&) { ++count; });
    bus.unsubscribe(id);
    bus.publish("ch", "x");
    REQUIRE(count == 0);
}

TEST_CASE("LocalEventBus does not cross-deliver between channels") {
    LocalEventBus bus;
    int count = 0;
    bus.subscribe("a", [&](const std::string&) { ++count; });
    bus.publish("b", "x");
    REQUIRE(count == 0);
}
