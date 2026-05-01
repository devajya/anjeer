#include <catch2/catch_test_macros.hpp>
#include "server/ws_server.h"
#include "server/auth_service.h"
#include "server/config.h"
#include "server/db.h"
#include "server/event_bus.h"
#include "server/lobby_gateway.h"
#include "server/lobby_repo.h"
#include "server/player_repo.h"

// POSIX networking
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using anjeer::server::ServerConfig;
using anjeer::server::WsServer;

// Shared singletons — pool_size=1 keeps the test DB connection count minimal.
static anjeer::server::DbPool& test_db_pool() {
    static anjeer::server::DbPool pool(TEST_DB_CONN, 1);
    return pool;
}
static anjeer::server::PlayerRepo& test_player_repo() {
    static anjeer::server::PlayerRepo repo;
    return repo;
}
static anjeer::server::LobbyRepo& test_lobby_repo() {
    static anjeer::server::LobbyRepo repo;
    return repo;
}
static anjeer::server::LocalEventBus& test_event_bus() {
    static anjeer::server::LocalEventBus bus;
    return bus;
}
static anjeer::server::LobbyGateway& test_lobby_gateway() {
    static anjeer::server::LobbyGateway gw(test_db_pool(), test_lobby_repo(), test_event_bus());
    return gw;
}

// ═══════════════════════════════════════════════════════════════════════════
// WsTestClient — minimal RFC 6455 text-frame WebSocket client
//
// Implements just enough of the protocol for smoke tests:
// - TCP connect + HTTP Upgrade handshake
// - Sending masked text frames (required for client→server by RFC 6455 §5.3)
// - Receiving unmasked server frames, auto-responding to pings
// No TLS — the test server runs plain HTTP.
// ═══════════════════════════════════════════════════════════════════════════

class WsTestClient {
public:
    explicit WsTestClient(int port) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
        if (fd_ < 0) throw std::runtime_error("socket() failed");

        // 3-second receive timeout — prevents tests hanging on missed messages.
        struct timeval tv{ 3, 0 };
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("connect() failed");

        do_handshake();
    }

    ~WsTestClient() { if (fd_ >= 0) ::close(fd_); }

    void send_text(const std::string& payload) {
        // FIN=1, opcode=TEXT(0x1), mask=1
        std::vector<uint8_t> frame;
        frame.push_back(0x81);

        const uint8_t mask[4] = { 0x1a, 0x2b, 0x3c, 0x4d };
        const size_t len = payload.size();
        if (len < 126) {
            frame.push_back(0x80 | static_cast<uint8_t>(len));
        } else if (len < 65536) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
            frame.push_back(static_cast<uint8_t>(len & 0xff));
        } else {
            throw std::runtime_error("payload too large for test client");
        }
        frame.insert(frame.end(), mask, mask + 4);
        for (size_t i = 0; i < len; ++i)
            frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);

        write_all(frame.data(), frame.size());
    }

    void send_json(const nlohmann::json& j) { send_text(j.dump()); }

    // Receive the next text/binary frame, automatically responding to pings.
    std::string recv_text() {
        for (;;) {
            uint8_t hdr[2];
            read_all(hdr, 2);

            const uint8_t op   = hdr[0] & 0x0f;
            const bool masked  = (hdr[1] & 0x80) != 0;
            size_t len         = hdr[1] & 0x7f;

            if (len == 126) {
                uint8_t ext[2]; read_all(ext, 2);
                len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
            } else if (len == 127) {
                throw std::runtime_error("64-bit length frames not supported in test client");
            }

            uint8_t mkey[4] = {};
            if (masked) read_all(mkey, 4);

            std::string payload(len, '\0');
            if (len) read_all(reinterpret_cast<uint8_t*>(payload.data()), len);
            if (masked) for (size_t i = 0; i < len; ++i) payload[i] ^= mkey[i % 4];

            if (op == 0x9) { send_pong(payload); continue; }  // ping → pong
            if (op == 0x8) throw std::runtime_error("server closed connection");
            if (op == 0x1 || op == 0x2) return payload;       // text or binary
            // skip continuation frames and unknown control frames
        }
    }

    nlohmann::json recv_json() { return nlohmann::json::parse(recv_text()); }

    // Drain incoming messages until one with the expected "type" field is found.
    // Skips unrelated message types (e.g. heartbeats) so tests are order-tolerant.
    nlohmann::json recv_of_type(const std::string& want) {
        for (int i = 0; i < 20; ++i) {
            auto j = recv_json();
            if (j.value("type", "") == want) return j;
        }
        throw std::runtime_error("did not receive message of type '" + want + "' within 20 frames");
    }

private:
    int fd_;

    void do_handshake() {
        // RFC 6455 example key — any valid base64 value is accepted by uWS.
        const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
        const std::string req =
            "GET /ws HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        write_all(reinterpret_cast<const uint8_t*>(req.data()), req.size());

        std::string resp;
        while (resp.find("\r\n\r\n") == std::string::npos) {
            char c;
            if (::recv(fd_, &c, 1, 0) <= 0) throw std::runtime_error("handshake recv failed");
            resp += c;
        }
        if (resp.find("101") == std::string::npos)
            throw std::runtime_error("expected 101 Switching Protocols, got: " + resp);
    }

    void send_pong(const std::string& payload) {
        std::vector<uint8_t> frame;
        frame.push_back(0x8a);  // FIN + PONG
        const uint8_t mask[4] = {};
        const size_t len = std::min(payload.size(), size_t{125});
        frame.push_back(0x80 | static_cast<uint8_t>(len));
        frame.insert(frame.end(), mask, mask + 4);
        for (size_t i = 0; i < len; ++i)
            frame.push_back(static_cast<uint8_t>(payload[i]));
        write_all(frame.data(), frame.size());
    }

    void write_all(const uint8_t* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            const ssize_t n = ::send(fd_, data + sent, len - sent, 0);
            if (n <= 0) throw std::runtime_error("send() failed");
            sent += static_cast<size_t>(n);
        }
    }

    void read_all(uint8_t* buf, size_t len) {
        size_t got = 0;
        while (got < len) {
            const ssize_t n = ::recv(fd_, buf + got, len - got, 0);
            if (n <= 0) throw std::runtime_error("recv() failed / timed out");
            got += static_cast<size_t>(n);
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Test server — started once per process, shared by all TEST_CASEs
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int WS_TEST_PORT  = 19002;
static constexpr int WS_LOBBY_PORT = 19007;

// Base config shared across all test servers. Callers override only what differs.
static ServerConfig make_test_server_config(int port) {
    ServerConfig cfg;
    cfg.host                  = "127.0.0.1";
    cfg.port                  = port;
    cfg.heartbeat_interval_ms = 60000;  // suppress noise during tests
    cfg.ping_interval_ms      = 1000;
    cfg.ping_timeout_ms       = 8000;
    cfg.order_book.min_price               = 1;
    cfg.order_book.max_price               = 99;
    cfg.order_book.nudge_initial_buy_price  = 1;
    cfg.order_book.nudge_initial_sell_price = 99;
    cfg.order_book.active_suits             = { "clubs" };
    cfg.game.player_count           = 2;
    cfg.game.total_cards            = 40;
    cfg.game.card_distribution      = {12, 10, 10, 8};
    cfg.game.countdown_seconds      = 0;
    cfg.game.round_duration_seconds = 3600;
    cfg.game.inter_round_seconds    = 5;
    cfg.scoring.starting_balance    = 100;
    cfg.scoring.round_buy_in_pct    = 0.20;
    cfg.scoring.points_per_card     = 20;
    cfg.db.connection_string        = TEST_DB_CONN;
    cfg.db.pool_size                = 1;
    cfg.db.migrations_dir           = TEST_MIGRATIONS_DIR;
    // AGENT-CTX: Auth config is required by WsServer to validate JWTs in the
    // upgrade handler. Test clients don't send cookies, so validate_access_token
    // is never called — but AuthService construction requires the fields.
    cfg.auth.jwt_secret                = "test-secret-must-be-at-least-32-chars!!";
    cfg.auth.access_token_ttl_seconds  = 3600;
    cfg.auth.refresh_token_ttl_seconds = 86400;
    cfg.auth.secure_cookies            = false;
    return cfg;
}

// AGENT-CTX: AuthService is a per-server singleton in tests, not shared across
// server instances (each test server uses make_test_server_config which returns
// a copy). test_auth_service() is only used where WS_TEST_PORT server is created.
static anjeer::server::AuthService& test_auth_service(const ServerConfig& cfg) {
    static anjeer::server::AuthService svc(test_db_pool(), test_player_repo(), cfg);
    return svc;
}

static void ensure_server_running() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        // Another call started it — wait until the port is accepting connections.
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(WS_TEST_PORT);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            const bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            ::close(fd);
            if (ok) return;
        }
        throw std::runtime_error("test server did not become ready in time");
    }

    // player_count=99 prevents any round from starting (tests never connect 99 clients).
    static ServerConfig cfg = make_test_server_config(WS_TEST_PORT);
    cfg.game.player_count      = 99;
    cfg.game.countdown_seconds = 3;

    std::thread([&cfg]() {
        WsServer srv(cfg, test_lobby_gateway(),
                     test_db_pool(), test_lobby_repo(),
                     test_auth_service(cfg), test_event_bus());
        srv.run();  // blocks; detached — OS cleans up on process exit
    }).detach();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(WS_TEST_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(fd);
        if (ok) return;
    }
    throw std::runtime_error("test server failed to start");
}

// ═══════════════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════════════

// AGENT-CTX: player_id is -1 for unauthenticated lobby sockets (no JWT cookie).
// The old architecture assigned slots (0,1,2,...) on connect; the new one only
// assigns slots when the player is in an active game session with a valid JWT.
TEST_CASE("WS server — player_hello sent on connect", "[ws_server]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    const auto hello = client.recv_of_type("player_hello");
    REQUIRE(hello.contains("player_id"));
    REQUIRE(hello["player_id"].is_number_integer());
    CHECK(hello["player_id"].get<int>() == -1);  // unauthenticated: no JWT cookie
}

// AGENT-CTX: submit_order / nudge / cancel_order on a lobby socket (not in a
// game session) must return ROUND_NOT_ACTIVE. These tests remain valid because
// the error guard in WsServer's message handler fires before any session lookup.
TEST_CASE("WS server — submit_order rejected when not in game session", "[ws_server][phase]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","submit_order"}, {"suit","clubs"}, {"side","buy"}, {"price",30} });

    const auto err = client.recv_of_type("error");
    REQUIRE(err.value("code","") == "ROUND_NOT_ACTIVE");
}

TEST_CASE("WS server — nudge rejected when not in game session", "[ws_server][phase]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","nudge"}, {"suit","clubs"}, {"side","buy"} });

    const auto err = client.recv_of_type("error");
    REQUIRE(err.value("code","") == "ROUND_NOT_ACTIVE");
}

TEST_CASE("WS server — cancel_order rejected when not in game session", "[ws_server][phase]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","cancel_order"}, {"order_id",1} });

    const auto err = client.recv_of_type("error");
    REQUIRE(err.value("code","") == "ROUND_NOT_ACTIVE");
}

// ═══════════════════════════════════════════════════════════════════════════
// Lobby subscription tests — subscribe_lobby / event forwarding
// ═══════════════════════════════════════════════════════════════════════════

struct LobbySetup { int64_t player_id; std::string lobby_id; };

// Insert a player (idempotent via ON CONFLICT) and create a fresh lobby via
// LobbyRepo so the 6-char code is generated correctly. Runs migrations so the
// test binary is self-contained even on a fresh anjeer_test DB.
static LobbySetup setup_lobby_in_db() {
    pqxx::connection conn(TEST_DB_CONN);
    anjeer::server::DbMigrator m(conn, TEST_MIGRATIONS_DIR);
    m.run();

    int64_t player_id;
    {
        pqxx::work txn(conn);
        auto r = txn.exec(
            "INSERT INTO players (username, oauth_provider, oauth_id) "
            "VALUES ('ws_sub_tester','github','gh_ws_sub_1') "
            "ON CONFLICT (oauth_provider, oauth_id) "
            "DO UPDATE SET username = excluded.username "
            "RETURNING id");
        player_id = r[0][0].as<int64_t>();
        txn.commit();
    }

    std::string lobby_id;
    {
        pqxx::work txn(conn);
        // Remove any leftover waiting lobby from a prior test run for this player.
        txn.exec_params(
            "DELETE FROM lobbies WHERE creator_id = $1 AND status = 'waiting'",
            player_id
        );
        const auto lobby = test_lobby_repo().create(txn, player_id, 2, 8);
        test_lobby_repo().add_player(txn, lobby.id, player_id);
        lobby_id = lobby.id;
        txn.commit();
    }
    return { player_id, lobby_id };
}

static anjeer::server::AuthService& test_lobby_auth_service() {
    static ServerConfig cfg = make_test_server_config(WS_LOBBY_PORT);
    static anjeer::server::AuthService svc(test_db_pool(), test_player_repo(), cfg);
    return svc;
}

static void ensure_lobby_sub_server_running() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{}; addr.sin_family = AF_INET;
            addr.sin_port = htons(WS_LOBBY_PORT);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            ::close(fd);
            if (ok) return;
        }
        throw std::runtime_error("WS lobby sub server did not become ready");
    }

    static ServerConfig cfg = make_test_server_config(WS_LOBBY_PORT);
    cfg.game.player_count      = 99;
    cfg.game.countdown_seconds = 3;

    std::thread([&cfg]() {
        WsServer srv(cfg, test_lobby_gateway(),
                     test_db_pool(), test_lobby_repo(),
                     test_lobby_auth_service(), test_event_bus());
        srv.run();
    }).detach();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{}; addr.sin_family = AF_INET;
        addr.sin_port = htons(WS_LOBBY_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(fd);
        if (ok) return;
    }
    throw std::runtime_error("WS lobby sub server failed to start");
}

TEST_CASE("WS server — subscribe_lobby returns lobby_state snapshot", "[ws_server][lobby]") {
    ensure_lobby_sub_server_running();
    const auto setup = setup_lobby_in_db();

    WsTestClient client(WS_LOBBY_PORT);
    client.recv_of_type("player_hello");

    client.send_json({ {"type","subscribe_lobby"}, {"lobby_id", setup.lobby_id} });

    const auto msg = client.recv_of_type("lobby_state");
    REQUIRE(msg.value("lobby_id", "") == setup.lobby_id);
    REQUIRE(msg.contains("code"));
    REQUIRE(msg["players"].is_array());
    CHECK(msg["players"].size() == 1);  // one player added in setup
}

TEST_CASE("WS server — subscribed client receives event_bus messages", "[ws_server][lobby]") {
    ensure_lobby_sub_server_running();
    const auto setup = setup_lobby_in_db();

    WsTestClient client(WS_LOBBY_PORT);
    client.recv_of_type("player_hello");

    client.send_json({ {"type","subscribe_lobby"}, {"lobby_id", setup.lobby_id} });
    client.recv_of_type("lobby_state");  // consume the initial snapshot

    // Publish directly to the shared event bus; recv_of_type waits up to 3 s.
    nlohmann::json ev;
    ev["type"]         = "player_joined";
    ev["lobby_id"]     = setup.lobby_id;
    ev["player_id"]    = setup.player_id;
    ev["username"]     = "ws_sub_tester";
    ev["player_count"] = 1;
    ev["joined_at"]    = "2026-04-23T00:00:00Z";
    test_event_bus().publish("lobby:" + setup.lobby_id, ev.dump());

    const auto fwd = client.recv_of_type("player_joined");
    CHECK(fwd.value("lobby_id",  "") == setup.lobby_id);
    CHECK(fwd.value("player_id", -1) == static_cast<int>(setup.player_id));
}
