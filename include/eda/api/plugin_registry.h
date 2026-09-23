#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "IPluginInteraction.h"

namespace eda {

struct StaticPluginEntry {
    std::string id;
    std::function<std::shared_ptr<IPluginInteraction>()> factory;
};

// 官方进程内插件的编译期注册表（等价于 HarnessPlan 的 plugin_registry.cpp 生成物）。
std::vector<StaticPluginEntry>& StaticPluginRegistry();

// 官方插件在各自 TU 内自注册。UniqueName 为 TU 内唯一标识符（用于生成变量名）；
// PluginClass 为可默认构造的具体插件类型。
//
// 注意：静态库中的本 TU 若无外部符号被引用，会被链接器整体丢弃，自注册不会执行。
// 宏因此额外生成一个**外部链接锚点** `eda_plugin_force_link_##UniqueName()`；
// 宿主（Composer）显式调用该锚点即可把本对象拉入链接（见 Composer::ForceLinkBuiltins）。
#define EDA_REGISTER_PLUGIN(UniqueName, PluginClass, PluginId)                          \
    namespace {                                                                         \
    const bool eda_registered_##UniqueName = [] {                                       \
        ::eda::StaticPluginRegistry().push_back(                                        \
            ::eda::StaticPluginEntry{                                                   \
                PluginId,                                                               \
                []() -> std::shared_ptr<::eda::IPluginInteraction> {                    \
                    return std::make_shared<PluginClass>();                             \
                }});                                                                    \
        return true;                                                                    \
    }();                                                                                \
    }                                                                                   \
    extern "C" void eda_plugin_force_link_##UniqueName() {                              \
        (void)eda_registered_##UniqueName;                                              \
    }

} // namespace eda
