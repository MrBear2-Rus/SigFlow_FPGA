#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>

#include "Types.h"

namespace eda {

// 核心服务基类标记：所有可被 ServiceRegistry 托管的服务从此派生。
class Service {
public:
    virtual ~Service() = default;
};

// 极简 DI 容器：按具体类型注册/查找。
// 注意：跨动态库边界不适用（受 R13 ABI 约束，官方插件编译进核心）。
class ServiceRegistry {
public:
    template <typename T, typename... Args>
    T& Add(Args&&... args) {
        static_assert(std::is_base_of_v<Service, T>, "T must derive from eda::Service");
        auto owned = std::make_shared<T>(std::forward<Args>(args)...);
        T& ref = *owned;
        services_[std::type_index(typeid(T))] = std::move(owned);
        return ref;
    }

    template <typename T>
    T* Find() const {
        const auto it = services_.find(std::type_index(typeid(T)));
        if (it == services_.end()) {
            return nullptr;
        }
        return static_cast<T*>(it->second.get());
    }

    template <typename T>
    T& Get() const {
        T* found = Find<T>();
        if (found == nullptr) {
            throw std::runtime_error(std::string("service not registered: ") + typeid(T).name());
        }
        return *found;
    }

    template <typename T>
    bool Remove() {
        return services_.erase(std::type_index(typeid(T))) > 0;
    }

    std::size_t Size() const { return services_.size(); }

private:
    std::unordered_map<std::type_index, std::shared_ptr<Service>> services_;
};

// 宿主上下文：持有服务注册表；后续 EventBus/Logger/SchemaRegistry 均以服务形式挂载。
class Context {
public:
    ServiceRegistry& services() { return services_; }
    const ServiceRegistry& services() const { return services_; }

private:
    ServiceRegistry services_;
};

} // namespace eda
