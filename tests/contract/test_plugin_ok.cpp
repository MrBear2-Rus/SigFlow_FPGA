#include <eda/api/abi.h>
#include <eda/api/build_info.h>

#include <cstdint>
#include <string>

namespace {

const char* BuildInfoCStr() {
    static const std::string value = eda::BuildInfo();
    return value.c_str();
}

int g_token = 0;

void RegisterPlugin(eda_host_api_v1*, void** out_instance) {
    *out_instance = &g_token;
}

void UnregisterPlugin(void*) {}

} // namespace

extern "C" EDA_PLUGIN_EXPORT const eda_plugin_descriptor_v1* eda_plugin_query_v1(
    std::uint32_t host_abi) {
    if (host_abi != EDA_PLUGIN_ABI_VERSION) return nullptr;
    static const eda_plugin_descriptor_v1 descriptor = [] {
        eda_plugin_descriptor_v1 d{};
        d.struct_size = sizeof(eda_plugin_descriptor_v1);
        d.abi_version = EDA_PLUGIN_ABI_VERSION;
        d.id = "test.ok";
        d.version = "1.0.0";
        d.manifest_json = "{\"id\":\"test.ok\",\"capabilities\":[\"synth/ok\"]}";
        d.build_info = BuildInfoCStr();
        d.register_plugin = RegisterPlugin;
        d.unregister_plugin = UnregisterPlugin;
        return d;
    }();
    return &descriptor;
}
