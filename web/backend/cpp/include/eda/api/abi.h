#pragma once

#include <cstdint>

#ifndef EDA_API
#define EDA_API
#endif

#if defined(_WIN32)
#define EDA_PLUGIN_EXPORT __declspec(dllexport)
#else
#define EDA_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define EDA_PLUGIN_ABI_VERSION 1u

extern "C" {

struct eda_host_api_v1; // opaque；P0-3/P0-4 定义实现

typedef struct eda_plugin_descriptor_v1 {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    const char* id;
    const char* version;
    const char* manifest_json;
    const char* build_info;
    void (*register_plugin)(struct eda_host_api_v1* host, void** out_instance);
    void (*unregister_plugin)(void* instance);
} eda_plugin_descriptor_v1;

EDA_PLUGIN_EXPORT const eda_plugin_descriptor_v1* eda_plugin_query_v1(std::uint32_t host_abi);

} // extern "C"
