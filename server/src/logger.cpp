#include "server/logger.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace anjeer::server {

Logger::Logger(const std::string& filepath) {
    // Create parent directories (e.g. logs/) if they don't exist yet.
    const auto parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    file_.open(filepath, std::ios::app);
    if (!file_.is_open()) {
        throw std::runtime_error("Logger: cannot open log file: " + filepath);
    }
}

void Logger::log(Level level, std::string_view component, std::string_view message) {
    const std::string line =
        now_iso() + " [" + level_str(level) + "][" +
        std::string(component) + "] " + std::string(message) + "\n";

    std::lock_guard<std::mutex> lk(mutex_);
    file_ << line;
    file_.flush();           // flush every line — debug logger, correctness over perf
    std::cout << line;
}

// static
std::string Logger::now_iso() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const auto t   = system_clock::to_time_t(now);

    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%S")
       << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
    return ss.str();
}

// static
const char* Logger::level_str(Level l) noexcept {
    switch (l) {
        case Level::DEBUG: return "DEBUG";
        case Level::INFO:  return "INFO ";
        case Level::WARN:  return "WARN ";
        case Level::ERROR: return "ERROR";
    }
    return "?????";
}

} // namespace anjeer::server
