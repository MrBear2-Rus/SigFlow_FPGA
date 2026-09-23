#include <eda/api/plugin_registry.h>

namespace eda {

std::vector<StaticPluginEntry>& StaticPluginRegistry() {
    static std::vector<StaticPluginEntry> registry;
    return registry;
}

} // namespace eda
