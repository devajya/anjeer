#include <catch2/catch_test_macros.hpp>
#include "server/api_key_repo.h"
#include "server/bot_manager.h"
#include "server/bot_scheduler.h"
#include "server/ws_server.h"
#include "server/auth_service.h"
#include "server/config.h"
#include "server/db.h"
#include "server/event_bus.h"
#include "server/lobby_gateway.h"
#include "server/lobby_repo.h"
#include "server/player_repo.h"
#include "server/reconnect_token_repo.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

// ─── Shared singletons ────────────────────────────────────────────────────────
// AGENT-CTX: Mirrors the ws_server_tests fixture pattern. RUN_SERIAL is set in
// CMakeLists.txt so this suite never runs in parallel with other port-binding tests.

static anjeer::server::DbPool& db() {
    static anjeer::server::DbPool pool(TEST_DB_CONN, 2);
    return pool;
}
static anjeer::server::LobbyRepo& lobby_repo() {
    static anjeer::server::LobbyRepo r; return r;
}
static anjeer::server::PlayerRepo& player_repo() {
    static anjeer::server::PlayerRepo r; return r;
}
static anjeer::server::LocalEventBus& event_bus() {
    static anjeer::server::LocalEventBus b; return b;
}
static anjeer::server::ApiKeyRepo& api_key_repo() {
    static anjeer::server::ApiKeyRepo r; return r;
}
static anjeer::server::LobbyGateway& lobby_gateway() {
    static anjeer::server::LobbyGateway gw(db(), lobby_repo(), event_bus());
    return gw;
}
static anjeer::server::BotManager& bot_manager() {
    static anjeer::server::BotScheduler sched(1);
    static anjeer::server::ServerConfig::BotsConfig bcfg;
    static anjeer::server::BotManager mgr(sched, bcfg);
    return mgr;
}

// ─── Reconnect tests ──────────────────────────────────────────────────────────

TEST_CASE("reconnect: within window — slot reattaches, snapshot received") {
    // TODO(T15): Start a 2-player game. Disconnect one player. Reconnect
    // within cfg_.reconnect.reconnect_window_seconds. Assert:
    // - server logs "slot X reattached"
    // - client receives game_state_snapshot message
}

TEST_CASE("reconnect: state snapshot contains correct hand after reattach") {
    // TODO(T15): After reattach, parse game_state_snapshot.hand and verify
    // it equals the hand the player held before disconnecting.
}

TEST_CASE("reconnect: reattach issues fresh token; old token is revoked") {
    // TODO(T15): After successful reattach, attempt to reconnect using the
    // OLD token — validate() must return nullopt (token was revoked).
}

TEST_CASE("reconnect: after window expiry — player_left broadcast sent") {
    // TODO(T15): Disconnect a player; wait > reconnect_window_seconds; verify
    // remaining players receive game_player_left broadcast.
}

TEST_CASE("reconnect: after window expiry — bot inherits hand") {
    // TODO(T15): After window expiry with spawn_bots_on_leave=true, verify
    // the replacement bot received game_bot_joined with the same slot_index.
}

TEST_CASE("reconnect: open orders cancelled on disconnect") {
    // TODO(T15): Place an order, disconnect the player, assert the order no
    // longer appears in book_update broadcast to the remaining player.
}

// ─── Queue tests ──────────────────────────────────────────────────────────────

TEST_CASE("queue: player admitted at round boundary fills expired slot") {
    // TODO(T15): Expire a slot, join_queue with a third player. On next round
    // start, assert queue_admitted arrives and the player can trade.
}

TEST_CASE("queue: queue overflow sends queue_overflow message") {
    // TODO(T15): Fill the queue to max_queue_size, then send one more
    // join_queue — assert the extra player receives queue_overflow.
}

TEST_CASE("queue: bot displacement on queue admit sends game_bot_replaced") {
    // TODO(T15): Start a game with a bot slot, fill queue, expire the bot's
    // slot, admit queue entry — verify game_bot_replaced broadcast.
}

TEST_CASE("queue: payout credited to disconnected player within window at round end") {
    // TODO(T15): Disconnect a player during RoundActive. Let the round end
    // while they are still within the reconnect window. Verify pending_payout
    // row in game_slots reflects the correct round payout.
}
