#include "server/ws_server.h"

#include <App.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>

namespace anjeer {
namespace server {

// AGENT-CTX: PerSocketData is the per-connection user-data struct uWS attaches to
// each WebSocket. Empty in Slice 1. Future slices add player ID, auth token, etc.
struct PerSocketData {};

// Convenience alias — uWS WebSocket type for this server (non-SSL, server-side).
using WsHandle = uWS::WebSocket<false, true, PerSocketData>*;

WsServer::WsServer(const ServerConfig& cfg) : cfg_(cfg) {}

void WsServer::run() {
    // All accesses to `connections` happen on the uWS event-loop thread:
    //   - open/close callbacks run on it directly
    //   - the heartbeat loop posts via Loop::defer(), which also runs on it
    // AGENT-CTX: Do not access `connections` from any other thread.
    std::set<WsHandle> connections;

    // Flag shared with the heartbeat thread. Written once (false) after run() returns.
    // AGENT-CTX: There is a small race window between the last running check and
    // the defer() call in the heartbeat thread. Acceptable for a dev server in Slice 1;
    // a proper shutdown mechanism (condition variable + wakeup) should be added if
    // graceful shutdown becomes a requirement.
    std::atomic<bool> running{true};

    // AGENT-CTX: uWS::App is created first so the event loop is initialised on this
    // thread before we capture Loop::get(). Reversing the order would return nullptr.
    uWS::App app;
    uWS::Loop* loop = uWS::Loop::get();

    app.ws<PerSocketData>("/ws", {
        // AGENT-CTX: sendPingsAutomatically=true makes uWS send a WS ping frame at
        // idleTimeout/2 seconds of silence and close the connection if no pong arrives
        // by idleTimeout seconds. This is the mechanism that satisfies the
        // "disconnection detected within 9 seconds" acceptance criterion.
        // idleTimeout is in whole seconds — ping_timeout_ms must be a multiple of 1000.
        // Default config sets ping_timeout_ms=9000 → idleTimeout=9, so uWS pings at
        // ~4.5s and closes at 9s if no pong. The client sees onclose and shows Disconnected.
        // AGENT-CTX: idleTimeout is unsigned short in uWS. The cast is safe because
        // ping_timeout_ms / 1000 will always be a small positive integer (≤ 30 in practice).
        .idleTimeout          = static_cast<unsigned short>(cfg_.ping_timeout_ms / 1000),
        .sendPingsAutomatically = true,

        .open = [&connections](WsHandle ws) {
            connections.insert(ws);
            std::cout << "[ws] client connected  total=" << connections.size() << "\n";
        },

        // AGENT-CTX: No application-level messages from client in Slice 1.
        // pong frames are consumed automatically by uWS — we never see them here.
        .message = [](WsHandle /*ws*/, std::string_view /*msg*/, uWS::OpCode /*op*/) {},

        .close = [&connections](WsHandle ws, int code, std::string_view /*msg*/) {
            connections.erase(ws);
            std::cout << "[ws] client disconnected  code=" << code
                      << "  total=" << connections.size() << "\n";
        }
    })
    .listen(cfg_.host, cfg_.port, [&](auto* token) {
        if (token) {
            std::cout << "[server] listening on "
                      << cfg_.host << ":" << cfg_.port << "\n";
        } else {
            std::cerr << "[server] failed to listen on port " << cfg_.port << "\n";
        }
    });

    // ---------------------------------------------------------------------------
    // Heartbeat thread
    // AGENT-CTX: Runs on a dedicated thread that sleeps heartbeat_interval_ms then
    // defers a broadcast onto the uWS event-loop thread. Loop::defer() uses an
    // internal eventfd/pipe to wake the loop — it IS safe to call from another thread.
    // The broadcast lambda captures connections by reference; this is safe because it
    // executes on the event-loop thread, not the heartbeat thread.
    // ---------------------------------------------------------------------------
    std::thread heartbeat_thread([&] {
        while (running) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg_.heartbeat_interval_ms));

            if (!running) break;

            const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            loop->defer([&connections, ts] {
                if (connections.empty()) return;

                const std::string payload =
                    nlohmann::json{{"type", "heartbeat"}, {"server_ts", ts}}.dump();

                for (WsHandle ws : connections) {
                    // AGENT-CTX: send() returns false if backpressure exceeds the
                    // limit set in ws options. We ignore this in Slice 1 — a heartbeat
                    // drop is acceptable. Future slices may add backpressure handling.
                    ws->send(payload, uWS::OpCode::TEXT);
                }
            });
        }
    });

    // Blocks until the event loop has no more work (i.e., until the process exits).
    // AGENT-CTX: In Slice 1, SIGINT kills the process via default signal handling.
    // app.run() will then return and we clean up the heartbeat thread.
    // A graceful shutdown (closing all connections before exiting) is deferred to
    // a later slice when it becomes operationally necessary.
    app.run();

    running = false;
    // The heartbeat thread may sleep up to heartbeat_interval_ms before noticing.
    heartbeat_thread.join();
}

} // namespace server
} // namespace anjeer
