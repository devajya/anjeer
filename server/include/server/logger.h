#pragma once

#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace anjeer::server {

// AGENT-CTX: Logger is a thin, thread-safe append-only file logger that also
// echoes every line to stdout so `make dev` remains readable.
// Two instances are created in ws_server.cpp:
//   - server_log  → logs/server_logs.txt  (WS lifecycle, messages in/out)
//   - engine_log  → logs/engine_logs.txt  (order-book operations + events)
// A third instance handles frontend entries forwarded via POST /api/log
//   - frontend_log → logs/frontend_logs.txt
//
// The Logger opens the file in *append* mode — entries survive server restarts
// within a dev session. Truncate manually when starting a fresh debug run.
class Logger {
public:
    enum class Level { DEBUG, INFO, WARN, ERROR };

    // Creates parent directory if absent, then opens filepath in append mode.
    // Throws std::runtime_error if the file cannot be opened.
    explicit Logger(const std::string& filepath);

    void log(Level level, std::string_view component, std::string_view message);

    // Convenience wrappers
    void debug(std::string_view comp, std::string_view msg) { log(Level::DEBUG, comp, msg); }
    void info (std::string_view comp, std::string_view msg) { log(Level::INFO,  comp, msg); }
    void warn (std::string_view comp, std::string_view msg) { log(Level::WARN,  comp, msg); }
    void error(std::string_view comp, std::string_view msg) { log(Level::ERROR, comp, msg); }

private:
    std::optional<std::ofstream> file_;
    bool       file_enabled_ = false;
    std::mutex mutex_;

    static std::string now_iso();
    static const char* level_str(Level l) noexcept;
};

} // namespace anjeer::server
