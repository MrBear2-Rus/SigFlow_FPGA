#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace sigflow {
namespace trace {

// 内存预算 LRU（W1-04）：按字节记账，超出上限按最近最少使用淘汰。
class TraceMemoryBudget {
public:
    explicit TraceMemoryBudget(std::size_t maxBytes = 64 * 1024 * 1024);

    void SetMaxBytes(std::size_t maxBytes);
    std::size_t MaxBytes() const { return m_maxBytes; }
    std::size_t UsedBytes() const { return m_usedBytes; }
    std::size_t EntryCount() const { return m_entries.size(); }
    bool Contains(const std::string& key) const;

    void Touch(const std::string& key);
    // 插入/更新条目；返回被淘汰的键（调用方负责删除对应缓存数据）。
    std::vector<std::string> Insert(const std::string& key, std::size_t bytes);
    void Erase(const std::string& key);
    void Clear();

private:
    std::vector<std::string> EvictToLimit();

    struct Entry {
        std::size_t bytes = 0;
        std::list<std::string>::iterator lru;
    };

    std::size_t m_maxBytes;
    std::size_t m_usedBytes = 0;
    std::list<std::string> m_lru;
    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace trace
} // namespace sigflow
