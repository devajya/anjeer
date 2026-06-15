#include "server/logger.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace anjeer::server {

Logger::Logger(const std::string& filepath) {
    const char* env = std::getenv("ANJEER_LOG_FILE");
    if (env && std::string(env) == "none") {
        return;
    }

    const auto parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    file_.emplace();
    file_->open(filepath, std::ios::app);
    if (!file_->is_open()) {
        file_.reset();
        throw std::runtime_error("Logger: cannot open log file: " + filepath);
    }
    file_enabled_ = true;
}

void Logger::log(Level level, std::string_view component, std::string_view message) {
    const std::string line =
        now_iso() + " [" + level_str(level) + "][" +
        std::string(component) + "] " + std::string(message) + "\n";

    std::lock_guard<std::mutex> lk(mutex_);
    if (file_enabled_) {
        (*file_) << line;
        file_->flush();
    }
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
