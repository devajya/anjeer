#include <catch2/catch_test_macros.hpp>

#include "server/api_key_repo.h"
#include "server/auth_service.h"
#include "server/config.h"
#include "server/db.h"
#include "server/event_bus.h"
#include "server/http_server.h"
#include "server/keybinds_repo.h"
#include "server/lobby_repo.h"
#include "server/player_repo.h"
#include "server/spectate_token_repo.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

using namespace anjeer::server;

// ─────────────────────────────────────────────────────────────────────────────
// Shared singletons — same instances used by the server thread and all tests
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int         k_port       = 18080;
static constexpr const char* k_jwt_secret = "test-jwt-secret-32-bytes-exactly!";

static ServerConfig& cfg() {
    static ServerConfig c = [] {
        ServerConfig s{};
        s.http_port                      = k_port;
        s.cors_origin                    = "http://localhost:5173";
        s.auth.jwt_secret                = k_jwt_secret;
        s.auth.access_token_ttl_seconds    = 900;
        s.auth.refresh_token_ttl_seconds   = 86400;
        s.auth.spectate_token_ttl_minutes  = 30;
        s.auth.secure_cookies              = false;
        // max_players=2: owner auto-joins on create, so one join fills the lobby.
        // min_players=2: one-player lobby is insufficient to start.
        s.lobby.min_players = 2;
        s.lobby.max_players = 2;
        return s;
    }();
    return c;
}

static DbPool&           dp()   { static DbPool           p(TEST_DB_CONN, 3); return p; }
static PlayerRepo&       pr()   { static PlayerRepo        r; return r; }
static LobbyRepo&        lr()   { static LobbyRepo         r; return r; }
static KeybindsRepo&     kr()   { static KeybindsRepo      r; return r; }
static ApiKeyRepo&       akr()  { static ApiKeyRepo        r; return r; }
static SpectateTokenRepo& str() { static SpectateTokenRepo r(30); return r; }
static LocalEventBus&    bus()  { static LocalEventBus     b; return b; }
static AuthService&      auth() { static AuthService       s(dp(), pr(), cfg()); return s; }

// ─────────────────────────────────────────────────────────────────────────────
// Minimal HTTP/1.1 test client — one TCP connection per call, Connection:close
// ─────────────────────────────────────────────────────────────────────────────

struct HttpResponse {
    int                                        status = 0;
    std::string                                body;
    std::unordered_map<std::string,std::string> headers;

    std::string error_code() const {
        try { return nlohmann::json::parse(body).value("error", ""); }
        catch (...) { return ""; }
    }
    std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it != headers.end() ? it->second : "";
    }
};

// Core send/receive — caller supplies the full request string.
static HttpResponse http_raw(const std::string& request_str)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(k_port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("http_raw() connect failed");
    }

    const char* p = request_str.data(); size_t left = request_str.size();
    while (left > 0) {
        ssize_t n = ::send(fd, p, left, 0);
        if (n <= 0) { ::close(fd); throw std::runtime_error("http_raw() send failed"); }
        p += n; left -= n;
    }

    std::string raw; char buf[4096]; ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) raw.append(buf, n);
    ::close(fd);

    HttpResponse res;
    const auto sp1 = raw.find(' ');
    if (sp1 != std::string::npos) {
        const auto sp2 = raw.find(' ', sp1 + 1);
        if (sp2 != std::string::npos)
            res.status = std::stoi(raw.substr(sp1 + 1, sp2 - sp1 - 1));
    }
    const auto hend = raw.find("\r\n\r\n");
    if (hend == std::string::npos) return res;

    // Parse response headers (lowercased names for case-insensitive lookup).
    const std::string header_block = raw.substr(0, hend);
    std::istringstream hss(header_block);
    std::string line;
    std::getline(hss, line); // skip status line
    while (std::getline(hss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // lowercase key
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // trim leading space from value
        if (!val.empty() && val.front() == ' ') val.erase(0, 1);
        res.headers[key] = val;
    }

    res.body = raw.substr(hend + 4);
    return res;
}

static HttpResponse http(const std::string& method,
                         const std::string& path,
                         const std::string& cookie = "",
                         const std::string& body   = "")
{
    std::string req = method + " " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\nConnection: close\r\n";
    if (!cookie.empty()) req += "Cookie: " + cookie + "\r\n";
    if (!body.empty())   req += "Content-Type: application/json\r\n"
                                "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    return http_raw(req);
}

static HttpResponse http_bearer(const std::string& method,
                                const std::string& path,
                                const std::string& api_key,
                                const std::string& body = "")
{
    std::string req = method + " " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\nConnection: close\r\n"
                      "Authorization: Bearer " + api_key + "\r\n";
    if (!body.empty())   req += "Content-Type: application/json\r\n"
                                "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    return http_raw(req);
}

// ─────────────────────────────────────────────────────────────────────────────
// Server lifecycle
// ─────────────────────────────────────────────────────────────────────────────

static void wait_for_port(int port, int tries = 200) {
    for (int i = 0; i < tries; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        ::close(fd);
        if (ok) return;
    }
    throw std::runtime_error("port " + std::to_string(port) + " not ready");
}

static void ensure_http_server_running() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) { wait_for_port(k_port); return; }

    std::thread([] {
        HttpServer srv(cfg(), anjeer::server::HttpServerDeps{
                           auth(), pr(), lr(), kr(), akr(), str(), bus(), dp()});
        srv.run();
    }).detach();

    wait_for_port(k_port);
}

// ─────────────────────────────────────────────────────────────────────────────
// DB helpers
// ─────────────────────────────────────────────────────────────────────────────

static void reset_db() {
    pqxx::connection conn(TEST_DB_CONN);
    DbMigrator m(conn, TEST_MIGRATIONS_DIR);
    m.run();
    pqxx::work txn(conn);
    txn.exec("TRUNCATE TABLE lobbies CASCADE");
    txn.exec("TRUNCATE TABLE players RESTART IDENTITY CASCADE");
    txn.commit();
}

struct TestPlayer { int64_t id; std::string cookie; };

static std::atomic<int> s_counter{0};

static TestPlayer make_player() {
    int n = ++s_counter;
    OAuthUserInfo info{ "gh_http_" + std::to_string(n),
                        "httpuser"  + std::to_string(n),
                        std::nullopt };
    const auto pl = auth().find_or_create("github", info);
    const auto tk = auth().issue_tokens(pl);
    return { pl.id, "access_token=" + tk.access_token };
}

static std::string lobby_id_from(const HttpResponse& res) {
    return nlohmann::json::parse(res.body)["id"].get<std::string>();
}

struct ApiKeyPlayer { int64_t id; std::string cookie; std::string api_key; };

static ApiKeyPlayer make_api_key_player() {
    const auto pl = make_player();
    std::string err;
    auto handle = dp().acquire();
    pqxx::work txn(handle.get());
    auto key = akr().create(txn, pl.id, "test-key", err);
    txn.commit();
    return { pl.id, pl.cookie, key.value_or("") };
}

static std::string lobby_code_from(const HttpResponse& res) {
    return nlohmann::json::parse(res.body)["code"].get<std::string>();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("HTTP lobby — unauthenticated requests return 401", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();

    CHECK(http("POST", "/lobbies").status        == 401);
    CHECK(http("GET",  "/lobbies").status        == 401);
    CHECK(http("POST", "/lobbies/x/join").status == 401);
    CHECK(http("POST", "/lobbies/x/start").status == 401);
}

TEST_CASE("HTTP lobby — POST /lobbies returns 201 with lobby JSON", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner = make_player();

    const auto res = http("POST", "/lobbies", owner.cookie);
    REQUIRE(res.status == 201);

    const auto j = nlohmann::json::parse(res.body);
    CHECK(j.value("status", "") == "waiting");
    CHECK(j["code"].get<std::string>().size() == 6);
    CHECK(j.value("player_count", 0) == 1);  // owner auto-joined
}

TEST_CASE("HTTP lobby — GET /lobbies lists waiting lobbies", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner = make_player();

    REQUIRE(http("POST", "/lobbies", owner.cookie).status == 201);

    const auto res = http("GET", "/lobbies", owner.cookie);
    REQUIRE(res.status == 200);

    const auto j = nlohmann::json::parse(res.body);
    REQUIRE(j["lobbies"].is_array());
    CHECK(j["lobbies"].size() == 1);
    CHECK(j["lobbies"][0].value("status", "") == "waiting");
}

TEST_CASE("HTTP lobby — successful join returns 200 and publishes player_joined", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner  = make_player();
    const auto joiner = make_player();

    const auto cr = http("POST", "/lobbies", owner.cookie);
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    std::string ev_payload;
    const uint64_t sub = bus().subscribe("lobby:" + lid,
        [&](const std::string& p) { ev_payload = p; });

    REQUIRE(http("POST", "/lobbies/" + lid + "/join", joiner.cookie).status == 200);
    bus().unsubscribe(sub);

    REQUIRE_FALSE(ev_payload.empty());
    const auto ev = nlohmann::json::parse(ev_payload);
    CHECK(ev.value("type",     "") == "player_joined");
    CHECK(ev.value("lobby_id", "") == lid);
    CHECK(ev.value("player_id",  -1) == static_cast<int>(joiner.id));
}

TEST_CASE("HTTP lobby — creating a second lobby while one is open returns 409 LOBBY_ALREADY_EXISTS", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner = make_player();

    REQUIRE(http("POST", "/lobbies", owner.cookie).status == 201);

    const auto res = http("POST", "/lobbies", owner.cookie);
    REQUIRE(res.status == 409);
    CHECK(res.error_code() == "LOBBY_ALREADY_EXISTS");
}

TEST_CASE("HTTP lobby — duplicate join is idempotent and returns 200", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner  = make_player();
    const auto joiner = make_player();

    const auto cr  = http("POST", "/lobbies", owner.cookie);
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    REQUIRE(http("POST", "/lobbies/" + lid + "/join", joiner.cookie).status == 200);

    // AGENT-CTX: Slice 7 resilience change — re-joining is idempotent.
    // Returns 200 with the original joined_at, not 409 ALREADY_JOINED.
    REQUIRE(http("POST", "/lobbies/" + lid + "/join", joiner.cookie).status == 200);
}

TEST_CASE("HTTP lobby — joining a full lobby returns 409 LOBBY_FULL", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner   = make_player();
    const auto joiner1 = make_player();
    const auto joiner2 = make_player();

    const auto cr  = http("POST", "/lobbies", owner.cookie);
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    // max_players=2; owner (1/2) + joiner1 fills it (2/2)
    REQUIRE(http("POST", "/lobbies/" + lid + "/join", joiner1.cookie).status == 200);

    const auto res = http("POST", "/lobbies/" + lid + "/join", joiner2.cookie);
    REQUIRE(res.status == 409);
    CHECK(res.error_code() == "LOBBY_FULL");
}

TEST_CASE("HTTP lobby — joining a non-waiting lobby returns 409 GAME_ALREADY_STARTED", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner   = make_player();
    const auto joiner1 = make_player();
    const auto joiner2 = make_player();

    const auto cr  = http("POST", "/lobbies", owner.cookie);
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    REQUIRE(http("POST", "/lobbies/" + lid + "/join",  joiner1.cookie).status == 200);
    REQUIRE(http("POST", "/lobbies/" + lid + "/start", owner.cookie).status   == 200);

    const auto res = http("POST", "/lobbies/" + lid + "/join", joiner2.cookie);
    REQUIRE(res.status == 409);
    CHECK(res.error_code() == "GAME_ALREADY_STARTED");
}

TEST_CASE("HTTP lobby — non-owner start returns 403 NOT_LOBBY_OWNER", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner    = make_player();
    const auto intruder = make_player();

    const auto cr  = http("POST", "/lobbies", owner.cookie);
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    const auto res = http("POST", "/lobbies/" + lid + "/start", intruder.cookie);
    REQUIRE(res.status == 403);
    CHECK(res.error_code() == "NOT_LOBBY_OWNER");
}

TEST_CASE("HTTP lobby — start with too few players returns 409 INSUFFICIENT_PLAYERS", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner = make_player();

    const auto cr  = http("POST", "/lobbies", owner.cookie);
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    // min_players=2; only owner is present (1 player)
    const auto res = http("POST", "/lobbies/" + lid + "/start", owner.cookie);
    REQUIRE(res.status == 409);
    CHECK(res.error_code() == "INSUFFICIENT_PLAYERS");
}

TEST_CASE("HTTP lobby — starting an already-started lobby returns 409 GAME_ALREADY_STARTED", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner  = make_player();
    const auto joiner = make_player();

    const auto cr  = http("POST", "/lobbies", owner.cookie);
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    REQUIRE(http("POST", "/lobbies/" + lid + "/join",  joiner.cookie).status == 200);
    REQUIRE(http("POST", "/lobbies/" + lid + "/start", owner.cookie).status  == 200);

    const auto res = http("POST", "/lobbies/" + lid + "/start", owner.cookie);
    REQUIRE(res.status == 409);
    CHECK(res.error_code() == "GAME_ALREADY_STARTED");
}

TEST_CASE("HTTP lobby — successful start returns 200 and publishes lobby_started", "[integration][http_lobby]") {
    ensure_http_server_running();
    reset_db();
    const auto owner  = make_player();
    const auto joiner = make_player();

    const auto cr  = http("POST", "/lobbies", owner.cookie);
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    REQUIRE(http("POST", "/lobbies/" + lid + "/join", joiner.cookie).status == 200);

    std::string ev_payload;
    const uint64_t sub = bus().subscribe("lobby:" + lid,
        [&](const std::string& p) { ev_payload = p; });

    const auto res = http("POST", "/lobbies/" + lid + "/start", owner.cookie);
    REQUIRE(res.status == 200);
    bus().unsubscribe(sub);

    REQUIRE_FALSE(ev_payload.empty());
    const auto ev = nlohmann::json::parse(ev_payload);
    CHECK(ev.value("type",     "") == "lobby_started");
    CHECK(ev.value("lobby_id", "") == lid);
}

// ─────────────────────────────────────────────────────────────────────────────
// Task 5 stubs — GET /lobbies/:code, mode filter, Bearer auth (RED before T6/T7)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("GET /lobbies/:code — known code returns LobbyView", "[integration][http_lobby][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto owner = make_player();
    const auto cr    = http("POST", "/lobbies", owner.cookie);
    REQUIRE(cr.status == 201);
    const auto code = lobby_code_from(cr);

    const auto res = http("GET", "/lobbies/" + code);
    REQUIRE(res.status == 200);
    const auto j = nlohmann::json::parse(res.body);
    CHECK(j["code"].get<std::string>() == code);
    CHECK(j.value("status", "") == "waiting");
}

TEST_CASE("GET /lobbies/:code — unknown code returns 404", "[integration][http_lobby][stub]") {
    ensure_http_server_running();
    reset_db();

    const auto res = http("GET", "/lobbies/ZZZZZZ");
    REQUIRE(res.status == 404);
    CHECK(res.error_code() == "NOT_FOUND");
}

TEST_CASE("GET /lobbies?mode=api — returns only API lobbies", "[integration][http_lobby][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto akp = make_api_key_player();

    // Create an API lobby via Bearer auth
    const auto cr = http_bearer("POST", "/lobbies",
                                akp.api_key,
                                R"({"mode":"api"})");
    REQUIRE(cr.status == 201);
    CHECK(nlohmann::json::parse(cr.body).value("mode","") == "api");

    // GET /lobbies?mode=api should return only API lobbies
    const auto list_res = http("GET", "/lobbies?mode=api", akp.cookie);
    REQUIRE(list_res.status == 200);
    const auto arr = nlohmann::json::parse(list_res.body)["lobbies"];
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() >= 1);
    for (const auto& l : arr) CHECK(l.value("mode","") == "api");
}

TEST_CASE("GET /lobbies?mode=ui — does not return API lobbies", "[integration][http_lobby][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto akp = make_api_key_player();
    const auto ui  = make_player();

    // Create an API lobby and a UI lobby
    REQUIRE(http_bearer("POST", "/lobbies", akp.api_key, R"({"mode":"api"})").status == 201);
    REQUIRE(http("POST", "/lobbies", ui.cookie).status == 201);

    const auto res = http("GET", "/lobbies?mode=ui", ui.cookie);
    REQUIRE(res.status == 200);
    const auto arr = nlohmann::json::parse(res.body)["lobbies"];
    REQUIRE(arr.is_array());
    for (const auto& l : arr) CHECK(l.value("mode","ui") == "ui");
}

TEST_CASE("POST /lobbies — Bearer auth creates API lobby", "[integration][http_lobby][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto akp = make_api_key_player();

    const auto res = http_bearer("POST", "/lobbies", akp.api_key, R"({"mode":"api"})");
    REQUIRE(res.status == 201);
    CHECK(nlohmann::json::parse(res.body).value("mode","") == "api");
}

TEST_CASE("POST /lobbies/:id/join — Bearer auth joins lobby", "[integration][http_lobby][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto akp1 = make_api_key_player();
    const auto akp2 = make_api_key_player();

    const auto cr  = http_bearer("POST", "/lobbies", akp1.api_key, R"({"mode":"api"})");
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    const auto res = http_bearer("POST", "/lobbies/" + lid + "/join", akp2.api_key);
    REQUIRE(res.status == 200);
}

TEST_CASE("POST /lobbies/:id/join — JWT auth on API lobby returns 403 LOBBY_MODE_MISMATCH",
          "[integration][http_lobby][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto akp     = make_api_key_player();
    const auto browser = make_player();

    const auto cr  = http_bearer("POST", "/lobbies", akp.api_key, R"({"mode":"api"})");
    REQUIRE(cr.status == 201);
    const auto lid = lobby_id_from(cr);

    const auto res = http("POST", "/lobbies/" + lid + "/join", browser.cookie);
    REQUIRE(res.status == 403);
    CHECK(res.error_code() == "LOBBY_MODE_MISMATCH");
}

// ─────────────────────────────────────────────────────────────────────────────
// Task 5 stubs — spectate token routes (RED before T7)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("POST /players/me/spectate-token — valid API key returns token",
          "[integration][spectate][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto akp = make_api_key_player();

    // Create a lobby so the code exists
    const auto cr = http_bearer("POST", "/lobbies", akp.api_key, R"({"mode":"api"})");
    REQUIRE(cr.status == 201);
    const auto code = lobby_code_from(cr);

    const auto body = R"({"lobby_code":")" + code + R"("})";
    const auto res  = http_bearer("POST", "/players/me/spectate-token", akp.api_key, body);
    REQUIRE(res.status == 200);
    const auto j = nlohmann::json::parse(res.body);
    REQUIRE(j.contains("token"));
    CHECK(j["token"].get<std::string>().substr(0, 4) == "stk_");
    CHECK(j.value("lobby_code","") == code);
}

TEST_CASE("POST /players/me/spectate-token — JWT auth returns 403",
          "[integration][spectate][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto player = make_player();

    const auto res = http("POST", "/players/me/spectate-token",
                          player.cookie,
                          R"({"lobby_code":"ABCDEF"})");
    REQUIRE(res.status == 403);
    CHECK(res.error_code() == "JWT_NOT_ALLOWED");
}

TEST_CASE("POST /players/me/spectate-token — unknown lobby_code returns 404",
          "[integration][spectate][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto akp = make_api_key_player();

    const auto res = http_bearer("POST", "/players/me/spectate-token",
                                 akp.api_key,
                                 R"({"lobby_code":"XXXXXX"})");
    REQUIRE(res.status == 404);
    CHECK(res.error_code() == "LOBBY_NOT_FOUND");
}

TEST_CASE("GET /auth/spectate — valid token sets Set-Cookie and returns 302",
          "[integration][spectate][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto akp = make_api_key_player();

    const auto cr = http_bearer("POST", "/lobbies", akp.api_key, R"({"mode":"api"})");
    REQUIRE(cr.status == 201);
    const auto code = lobby_code_from(cr);

    const auto body = R"({"lobby_code":")" + code + R"("})";
    const auto tr   = http_bearer("POST", "/players/me/spectate-token", akp.api_key, body);
    REQUIRE(tr.status == 200);
    const auto token = nlohmann::json::parse(tr.body)["token"].get<std::string>();

    const auto res = http("GET", "/auth/spectate?token=" + token);
    REQUIRE(res.status == 302);
    CHECK_FALSE(res.header("set-cookie").empty());
    CHECK(res.header("location").find("/spectate/") != std::string::npos);
}

TEST_CASE("GET /auth/spectate — already-used token returns 401",
          "[integration][spectate][stub]") {
    ensure_http_server_running();
    reset_db();
    const auto akp = make_api_key_player();

    const auto cr = http_bearer("POST", "/lobbies", akp.api_key, R"({"mode":"api"})");
    REQUIRE(cr.status == 201);
    const auto code = lobby_code_from(cr);

    const auto body = R"({"lobby_code":")" + code + R"("})";
    const auto tr   = http_bearer("POST", "/players/me/spectate-token", akp.api_key, body);
    REQUIRE(tr.status == 200);
    const auto token = nlohmann::json::parse(tr.body)["token"].get<std::string>();

    // First use — consumes the token
    REQUIRE(http("GET", "/auth/spectate?token=" + token).status == 302);
    // Second use — token is already marked used
    REQUIRE(http("GET", "/auth/spectate?token=" + token).status == 401);
}

TEST_CASE("GET /auth/spectate — invalid token returns 401",
          "[integration][spectate][stub]") {
    ensure_http_server_running();
    reset_db();

    const auto res = http("GET", "/auth/spectate?token=stk_notarealtoken");
    REQUIRE(res.status == 401);
    CHECK(res.error_code() == "TOKEN_INVALID");
}

