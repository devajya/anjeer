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

static constexpr int WS_TEST_PORT  = 19002;
static constexpr int WS_ROUND_PORT = 19003;

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
    cfg.game.player_count      = 99;
    cfg.game.total_cards       = 40;
    cfg.game.card_distribution = {12, 10, 10, 8};
    cfg.game.countdown_seconds = 3;

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
    cfg.game.player_count      = 2;
    cfg.game.total_cards       = 40;
    cfg.game.card_distribution = {12, 10, 10, 8};
    cfg.game.countdown_seconds = 0;

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
