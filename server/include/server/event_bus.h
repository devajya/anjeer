#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace anjeer::server {

using EventHandler = std::function<void(const std::string& payload)>;

class IEventBus {
public:
    virtual ~IEventBus() = default;
    virtual void     publish    (const std::string& channel,
                                 const std::string& payload) = 0;
    virtual uint64_t subscribe  (const std::string& channel,
                                 EventHandler handler) = 0;
    virtual void     unsubscribe(uint64_t sub_id) = 0;
};

// In-process delivery. Handlers called synchronously on the publish thread.
// Correct for single-node (local dev). No external dependencies.
class LocalEventBus : public IEventBus {
public:
    void     publish    (const std::string& channel,
                         const std::string& payload) override;
    uint64_t subscribe  (const std::string& channel,
                         EventHandler handler) override;
    void     unsubscribe(uint64_t sub_id) override;
private:
    std::unordered_map<std::string, std::unordered_map<uint64_t, EventHandler>> by_channel_;
    std::unordered_map<uint64_t, std::string> sub_channels_;
    uint64_t next_id_{ 0 };
};

// TODO Slice 16: RedisEventBus — wraps Upstash PUBLISH/SUBSCRIBE.
class RedisEventBus : public IEventBus {
public:
    void     publish    (const std::string&, const std::string&) override {}
    uint64_t subscribe  (const std::string&, EventHandler)       override { return 0; }
    void     unsubscribe(uint64_t)                               override {}
};

} // namespace anjeer::server
