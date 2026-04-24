#include "server/event_bus.h"

namespace anjeer::server {

void LocalEventBus::publish(const std::string& channel, const std::string& payload) {
    auto it = by_channel_.find(channel);
    if (it == by_channel_.end()) return;
    for (auto& [id, handler] : it->second) {
        handler(payload);
    }
}

uint64_t LocalEventBus::subscribe(const std::string& channel, EventHandler handler) {
    uint64_t id = next_id_++;
    by_channel_[channel][id] = std::move(handler);
    sub_channels_[id] = channel;
    return id;
}

void LocalEventBus::unsubscribe(uint64_t sub_id) {
    auto it = sub_channels_.find(sub_id);
    if (it == sub_channels_.end()) return;
    auto& inner = by_channel_[it->second];
    inner.erase(sub_id);
    if (inner.empty()) by_channel_.erase(it->second);
    sub_channels_.erase(it);
}

} // namespace anjeer::server
