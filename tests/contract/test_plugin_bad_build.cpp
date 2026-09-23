#include <eda/api/abi.h>

#include <cstdint>

namespace {

void RegisterPlugin(eda_host_api_v1*, void** out_instance) {
    *out_instance = nullptr;
}

void UnregisterPlugin(void*) {}

} // namespace

// 故意回传与宿主不一致的构建指纹：宿主必须拒绝加载并给出原因（修 R13）。
extern "C" EDA_PLUGIN_EXPORT const eda_plugin_descriptor_v1* eda_plugin_query_v1(
    std::uint32_t host_abi) {
    if (host_abi != EDA_PLUGIN_ABI_VERSION) return nullptr;
    static const eda_plugin_descriptor_v1 descriptor = [] {
        eda_plugin_descriptor_v1 d{};
        d.struct_size = sizeof(eda_plugin_descriptor_v1);
        d.abi_version = EDA_PLUGIN_ABI_VERSION;
        d.id = "test.bad_build";
        d.version = "1.0.0";
        d.manifest_json = "{\"id\":\"test.bad_build\"}";
        d.build_info = "bogus-build-info";
        d.register_plugin = RegisterPlugin;
        d.unregister_plugin = UnregisterPlugin;
        return d;
    }();
    return &descriptor;
}
