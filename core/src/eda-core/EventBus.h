#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <eda/api/events.hpp>

namespace eda {

// 进程内同步事件总线。线程编组（投递到 UI 线程）由宿主在订阅端负责；
// Publish 在锁外调用 handler，允许 handler 内再次 Subscribe/Unsubscribe。
class InProcessEventBus final : public EventBus {
public:
    EventHandlerId Subscribe(const std::string& topic, Handler handler) override;
    void Unsubscribe(EventHandlerId id) override;
    void Publish(const std::string& topic, const Json& payload) override;

    std::size_t SubscriberCount(const std::string& topic) const;

private:
    struct Entry {
        EventHandlerId id = 0;
        Handler handler;
    };

    mutable std::mutex mutex_;
    EventHandlerId nextId_ = 1;
    std::unordered_map<std::string, std::vector<Entry>> handlers_;
    std::unordered_map<EventHandlerId, std::string> idToTopic_;
};

} // namespace eda
