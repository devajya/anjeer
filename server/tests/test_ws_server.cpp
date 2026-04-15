#include <catch2/catch_test_macros.hpp>
#include "server/ws_server.h"
#include "server/config.h"

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

// ═══════════════════════════════════════════════════════════════════════════
// WsTestClient — minimal RFC 6455 text-frame WebSocket client
// ═══════════════════════════════════════════════════════════════════════════
//
// AGENT-CTX: Implements just enough of the protocol for smoke tests:
// - TCP connect + HTTP Upgrade handshake
// - Sending masked text frames (required for client→server by RFC 6455 §5.3)
// - Receiving unmasked server frames, auto-responding to pings
// No TLS — the test server runs plain HTTP.

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
//
// AGENT-CTX: WsServer::run() blocks indefinitely. We detach it in a background
// thread and let the process exit naturally when Catch2 finishes — the OS reclaims
// the thread. The static atomic ensures only one server instance binds the port,
// even if multiple TEST_CASEs call ensure_server_running() concurrently.

static constexpr int WS_TEST_PORT    = 19002;
static constexpr int WS_ROUND_PORT   = 19003;
static constexpr int WS_TIMER_PORT   = 19004;
static constexpr int WS_DISCONN_PORT = 19005;
// AGENT-CTX: WS_ENDED_PORT hosts a server that expires after 1 s and transitions
// to Ended phase. Used exclusively by "order rejected after round ends" — needs its
// own port because Ended phase is terminal (no new rounds start on this server).
static constexpr int WS_ENDED_PORT   = 19006;

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

    ServerConfig cfg;
    cfg.host                  = "127.0.0.1";
    cfg.port                  = WS_TEST_PORT;
    cfg.heartbeat_interval_ms = 60000;  // suppress noise during tests
    cfg.ping_interval_ms      = 1000;
    cfg.ping_timeout_ms       = 8000;
    cfg.order_book.min_price               = 1;
    cfg.order_book.max_price               = 99;
    cfg.order_book.nudge_initial_buy_price  = 1;
    cfg.order_book.nudge_initial_sell_price = 99;
    cfg.order_book.active_suits             = { "clubs" };
    // AGENT-CTX: player_count=99 prevents any round from starting during tests
    // (tests connect 1-2 clients, never reaching 99).
    cfg.game.player_count           = 99;
    cfg.game.total_cards            = 40;
    cfg.game.card_distribution      = {12, 10, 10, 8};
    cfg.game.countdown_seconds      = 3;
    cfg.game.round_duration_seconds = 3600;
    cfg.scoring.starting_balance    = 100;
    cfg.scoring.buy_in              = 50;
    cfg.scoring.points_per_card     = 20;

    std::thread([cfg]() {
        WsServer srv(cfg);
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

// AGENT-CTX: ensure_round_server_running starts a second server (port 19003)
// with player_count=2 and countdown_seconds=0. Two primer clients connect to
// trigger the round immediately, then disconnect. Subsequent test clients
// connect into an already-Active server and can place orders freely.
// Tests that need Active phase must use WS_ROUND_PORT, not WS_TEST_PORT.
// Tests that need Waiting phase (ROUND_NOT_ACTIVE checks) use WS_TEST_PORT.
static void ensure_round_server_running() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(WS_ROUND_PORT);
            ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            const bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            ::close(fd);
            if (ok) return;
        }
        throw std::runtime_error("round test server did not become ready in time");
    }

    ServerConfig cfg;
    cfg.host                  = "127.0.0.1";
    cfg.port                  = WS_ROUND_PORT;
    cfg.heartbeat_interval_ms = 60000;
    cfg.ping_interval_ms      = 1000;
    cfg.ping_timeout_ms       = 8000;
    cfg.order_book.min_price               = 1;
    cfg.order_book.max_price               = 99;
    cfg.order_book.nudge_initial_buy_price  = 1;
    cfg.order_book.nudge_initial_sell_price = 99;
    cfg.order_book.active_suits             = { "clubs" };
    // AGENT-CTX: player_count=2, countdown_seconds=0 so the primer (two
    // dummy clients) triggers an immediate deal. After primer disconnects,
    // the server stays in Active phase for all order-related tests.
    cfg.game.player_count           = 2;
    cfg.game.total_cards            = 40;
    cfg.game.card_distribution      = {12, 10, 10, 8};
    cfg.game.countdown_seconds      = 0;
    cfg.game.round_duration_seconds = 3600;
    cfg.scoring.starting_balance    = 100;
    cfg.scoring.buy_in              = 50;
    cfg.scoring.points_per_card     = 20;

    std::thread([cfg]() {
        WsServer srv(cfg);
        srv.run();
    }).detach();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(WS_ROUND_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(fd);
        if (ok) break;
        if (i == 199) throw std::runtime_error("round server failed to start");
    }

    // Primer: connect two clients to trigger begin_countdown → immediate deal.
    // Both clients wait for round_start before disconnecting so the server is
    // definitely in Active phase when this function returns.
    {
        WsTestClient c0(WS_ROUND_PORT);
        c0.recv_of_type("player_hello");
        WsTestClient c1(WS_ROUND_PORT);
        c1.recv_of_type("player_hello");
        // countdown_seconds=0 → round_starting arrives immediately after c1 joins
        c0.recv_of_type("round_starting");
        c1.recv_of_type("round_starting");
        // deal fires on next loop iteration
        c0.recv_of_type("round_start");
        c1.recv_of_type("round_start");
    }  // c0, c1 disconnect here; server remains Active; slots freed for tests
}

// ═══════════════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("WS server — player_hello sent on connect", "[ws_server]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    const auto hello = client.recv_of_type("player_hello");
    REQUIRE(hello.contains("player_id"));
    REQUIRE(hello["player_id"].is_number_integer());
    CHECK(hello["player_id"].get<int>() >= 0);  // 0-indexed slots
}

TEST_CASE("WS server — book_update snapshot sent on connect", "[ws_server]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    client.recv_of_type("player_hello");
    const auto upd = client.recv_of_type("book_update");
    CHECK(upd.value("suit", "") == "clubs");
    CHECK(upd.contains("best_bid"));
    CHECK(upd.contains("best_ask"));
}

TEST_CASE("WS server — submit_order returns order_ack", "[ws_server]") {
    ensure_round_server_running();
    WsTestClient client(WS_ROUND_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","submit_order"}, {"suit","clubs"}, {"side","buy"}, {"price",30} });

    const auto ack = client.recv_of_type("order_ack");
    REQUIRE(ack.value("suit","") == "clubs");
    REQUIRE(ack.value("side","") == "buy");
    REQUIRE(ack.value("price",0) == 30);
    REQUIRE(ack.contains("order_id"));

    // Cancel the order to leave the book clean for subsequent tests.
    client.send_json({ {"type","cancel_order"}, {"order_id", ack["order_id"]} });
    client.recv_of_type("order_cancel_ack");
}

TEST_CASE("WS server — unknown suit returns UNKNOWN_SUIT error", "[ws_server]") {
    ensure_round_server_running();
    WsTestClient client(WS_ROUND_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","submit_order"}, {"suit","BOGUS"}, {"side","buy"}, {"price",50} });

    const auto err = client.recv_of_type("error");
    REQUIRE(err.value("code","") == "UNKNOWN_SUIT");
}

TEST_CASE("WS server — crossing orders produce trade then global book wipe", "[ws_server]") {
    // AGENT-CTX: This test is order-sensitive and should run last in the file.
    // A trade triggers a global wipe of all books, leaving a clean state.
    ensure_round_server_running();

    WsTestClient buyer(WS_ROUND_PORT);
    WsTestClient seller(WS_ROUND_PORT);

    // Each client receives hello + initial book snapshot on connect.
    buyer.recv_of_type("player_hello");
    buyer.recv_of_type("book_update");
    seller.recv_of_type("player_hello");
    seller.recv_of_type("book_update");

    // Post a resting bid; both clients receive the resulting book_update.
    buyer.send_json({ {"type","submit_order"}, {"suit","clubs"}, {"side","buy"}, {"price",50} });
    buyer.recv_of_type("order_ack");
    buyer.recv_of_type("book_update");   // broadcast: best_bid=50
    seller.recv_of_type("book_update");  // same broadcast received by seller

    // Aggress with a crossing sell — should match at the resting (maker) price.
    seller.send_json({ {"type","submit_order"}, {"suit","clubs"}, {"side","sell"}, {"price",50} });
    seller.recv_of_type("order_ack");

    // Both clients receive the trade notification.
    const auto trade_b = buyer.recv_of_type("trade");
    const auto trade_s = seller.recv_of_type("trade");
    REQUIRE(trade_b.value("suit","")  == "clubs");
    REQUIRE(trade_b.value("price", 0) == 50);
    REQUIRE(trade_s.value("suit","")  == "clubs");

    // After any trade, all books are wiped — both clients see null bid and ask.
    const auto wipe_b = buyer.recv_of_type("book_update");
    const auto wipe_s = seller.recv_of_type("book_update");
    REQUIRE(wipe_b["best_bid"].is_null());
    REQUIRE(wipe_b["best_ask"].is_null());
    REQUIRE(wipe_s["best_bid"].is_null());
    REQUIRE(wipe_s["best_ask"].is_null());
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase gate tests — Slice 4 bug fix
// AGENT-CTX: These tests use the Waiting-phase server (WS_TEST_PORT,
// player_count=99) so the round never starts and orders must be rejected.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("WS server — submit_order rejected before round is active", "[ws_server][phase]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","submit_order"}, {"suit","clubs"}, {"side","buy"}, {"price",30} });

    const auto err = client.recv_of_type("error");
    REQUIRE(err.value("code","") == "ROUND_NOT_ACTIVE");
}

TEST_CASE("WS server — nudge rejected before round is active", "[ws_server][phase]") {
    ensure_server_running();
    WsTestClient client(WS_TEST_PORT);

    client.recv_of_type("player_hello");

    client.send_json({ {"type","nudge"}, {"suit","clubs"}, {"side","buy"} });

    const auto err = client.recv_of_type("error");
    REQUIRE(err.value("code","") == "ROUND_NOT_ACTIVE");
}

// ═══════════════════════════════════════════════════════════════════════════
// Round timer tests — Slice 4
//
// AGENT-CTX: Each timer test gets its own server port (WS_TIMER_PORT /
// WS_DISCONN_PORT) because after the round expires the server transitions to
// Ended and cannot host a second round. Sharing a port across tests would
// leave the second test connecting into an Ended-phase server.
// ═══════════════════════════════════════════════════════════════════════════

static void start_server_on_port(int port) {
    ServerConfig cfg;
    cfg.host                        = "127.0.0.1";
    cfg.port                        = port;
    cfg.heartbeat_interval_ms       = 60000;
    cfg.ping_interval_ms            = 1000;
    cfg.ping_timeout_ms             = 8000;
    cfg.order_book.min_price               = 1;
    cfg.order_book.max_price               = 99;
    cfg.order_book.nudge_initial_buy_price  = 1;
    cfg.order_book.nudge_initial_sell_price = 99;
    cfg.order_book.active_suits             = { "clubs" };
    cfg.game.player_count           = 2;
    cfg.game.total_cards            = 40;
    cfg.game.card_distribution      = {12, 10, 10, 8};
    cfg.game.countdown_seconds      = 0;
    cfg.game.round_duration_seconds = 1;
    cfg.scoring.starting_balance    = 100;
    cfg.scoring.buy_in              = 50;
    cfg.scoring.points_per_card     = 20;

    std::thread([cfg]() { WsServer srv(cfg); srv.run(); }).detach();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ::close(fd);
        if (ok) return;
    }
    throw std::runtime_error("timer test server failed to start on port " + std::to_string(port));
}

TEST_CASE("WS server — round_start includes round_end_at and round_end received", "[ws_server][timer]") {
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) start_server_on_port(WS_TIMER_PORT);

    WsTestClient c0(WS_TIMER_PORT);
    WsTestClient c1(WS_TIMER_PORT);

    c0.recv_of_type("player_hello");
    c1.recv_of_type("player_hello");

    // countdown=0 → round_starting fires immediately; deal follows on next loop tick
    c0.recv_of_type("round_starting");
    c1.recv_of_type("round_starting");

    const auto rs0 = c0.recv_of_type("round_start");
    const auto rs1 = c1.recv_of_type("round_start");

    REQUIRE(rs0.contains("round_end_at"));
    REQUIRE(rs1.contains("round_end_at"));
    CHECK(rs0["round_end_at"].is_string());
    CHECK(rs0["round_end_at"].get<std::string>().size() >= 20);  // ISO 8601 sanity

    // Wait for round_end (round_duration_seconds=1; allow 3 s of slack).
    const auto re0 = c0.recv_of_type("round_end");
    const auto re1 = c1.recv_of_type("round_end");

    REQUIRE(re0.contains("goal_suit"));
    REQUIRE(re0.contains("results"));
    CHECK(re0["results"].is_array());
    CHECK(re0["results"].size() == 2);

    // Both clients receive the same goal_suit and results array.
    CHECK(re0["goal_suit"] == re1["goal_suit"]);
    CHECK(re0["results"]   == re1["results"]);

    // Both connected players are marked not disconnected.
    for (const auto& pr : re0["results"]) {
        CHECK(pr["disconnected"] == false);
    }
}

TEST_CASE("WS server — books wiped when round expires", "[ws_server][timer]") {
    // AGENT-CTX: Connects to the same WS_TIMER_PORT server after the round has
    // already started (and likely already ended). We verify that the round_end
    // sequence includes book_update messages with null bid/ask (from the wipe)
    // by reusing the client that triggers the round in the previous test.
    // This test is a structural check on the order of messages: wipe before
    // round_end. Because Catch2 does not guarantee test ordering, this test
    // is self-contained and starts its own server at a new port if needed.
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) start_server_on_port(WS_TIMER_PORT + 10);

    WsTestClient c0(WS_TIMER_PORT + 10);
    WsTestClient c1(WS_TIMER_PORT + 10);

    c0.recv_of_type("player_hello");
    c1.recv_of_type("player_hello");
    c0.recv_of_type("round_starting");
    c1.recv_of_type("round_starting");
    c0.recv_of_type("round_start");
    c1.recv_of_type("round_start");

    // Place a resting bid so there is an order to wipe.
    c0.send_json({ {"type","submit_order"}, {"suit","clubs"}, {"side","buy"}, {"price",30} });
    c0.recv_of_type("order_ack");
    c0.recv_of_type("book_update");  // broadcast: best_bid=30
    c1.recv_of_type("book_update");

    // Wait for expiry. The wipe sends null book_updates before round_end.
    const auto wipe = c0.recv_of_type("book_update");
    REQUIRE(wipe["best_bid"].is_null());
    REQUIRE(wipe["best_ask"].is_null());

    // round_end follows the wipe.
    REQUIRE(c0.recv_of_type("round_end").contains("goal_suit"));
}

// AGENT-CTX: Scoring phase is entered and exited in microseconds (score_round +
// dispatch are synchronous on the event-loop thread). Testing the Scoring phase
// directly would require injecting a delay inside score_round, which is impractical.
// Instead we test the Ended phase immediately after round_end — both phases use the
// same guard (round_phase != Active) so the rejection code path is identical.
TEST_CASE("WS server — order rejected after round ends (Ended phase)", "[ws_server][phase]") {
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) start_server_on_port(WS_ENDED_PORT);

    WsTestClient c0(WS_ENDED_PORT);
    WsTestClient c1(WS_ENDED_PORT);

    c0.recv_of_type("player_hello");
    c1.recv_of_type("player_hello");
    c0.recv_of_type("round_starting");
    c1.recv_of_type("round_starting");
    c0.recv_of_type("round_start");
    c1.recv_of_type("round_start");

    // round_duration_seconds=1 — wait for expiry.
    c0.recv_of_type("round_end");

    // Server is now in Ended phase; all order commands must return ROUND_NOT_ACTIVE.
    c0.send_json({ {"type","submit_order"}, {"suit","clubs"}, {"side","buy"}, {"price",30} });
    const auto err = c0.recv_of_type("error");
    REQUIRE(err.value("code","") == "ROUND_NOT_ACTIVE");
}

TEST_CASE("WS server — disconnected player excluded from round_end delivery", "[ws_server][round_end]") {
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) start_server_on_port(WS_DISCONN_PORT);

    WsTestClient c0(WS_DISCONN_PORT);
    {
        WsTestClient c1(WS_DISCONN_PORT);

        c0.recv_of_type("player_hello");
        c1.recv_of_type("player_hello");
        c0.recv_of_type("round_starting");
        c1.recv_of_type("round_starting");
        c0.recv_of_type("round_start");
        c1.recv_of_type("round_start");

        // c1 goes out of scope here → TCP close → server nulls player_slots[1]
    }

    // c0 still connected. Wait for round_end (~1 s expiry + 3 s recv timeout).
    const auto re = c0.recv_of_type("round_end");
    REQUIRE(re.contains("results"));

    // The disconnected player (slot 1) must be marked disconnected in results.
    bool found_disconnected = false;
    for (const auto& pr : re["results"]) {
        if (pr["disconnected"].get<bool>()) {
            found_disconnected = true;
            break;
        }
    }
    CHECK(found_disconnected);
}
