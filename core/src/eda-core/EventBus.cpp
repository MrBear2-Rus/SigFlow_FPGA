#include "EventBus.h"

#include <algorithm>
#include <vector>

namespace eda {

EventHandlerId InProcessEventBus::Subscribe(const std::string& topic, Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    const EventHandlerId id = nextId_++;
    handlers_[topic].push_back(Entry{id, std::move(handler)});
    idToTopic_[id] = topic;
    return id;
}

void InProcessEventBus::Unsubscribe(EventHandlerId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto topicIt = idToTopic_.find(id);
    if (topicIt == idToTopic_.end()) {
        return;
    }
    const std::string topic = topicIt->second;
    idToTopic_.erase(topicIt);
    const auto listIt = handlers_.find(topic);
    if (listIt == handlers_.end()) {
        return;
    }
    auto& entries = listIt->second;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [id](const Entry& e) { return e.id == id; }),
                  entries.end());
    if (entries.empty()) {
        handlers_.erase(listIt);
    }
}

void InProcessEventBus::Publish(const std::string& topic, const Json& payload) {
    std::vector<Handler> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = handlers_.find(topic);
        if (it == handlers_.end()) {
            return;
        }
        snapshot.reserve(it->second.size());
        for (const auto& entry : it->second) {
            snapshot.push_back(entry.handler);
        }
    }
    for (const auto& handler : snapshot) {
        if (handler) {
            handler(payload);
        }
    }
}

std::size_t InProcessEventBus::SubscriberCount(const std::string& topic) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = handlers_.find(topic);
    return it == handlers_.end() ? 0 : it->second.size();
}

} // namespace eda
