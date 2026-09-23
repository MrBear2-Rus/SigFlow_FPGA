#include <eda/api/abi.h>

#include <cstdint>

// 故意拒绝宿主 ABI 握手：宿主必须记录"ABI handshake rejected"（修 R1）。
extern "C" EDA_PLUGIN_EXPORT const eda_plugin_descriptor_v1* eda_plugin_query_v1(
    std::uint32_t /*host_abi*/) {
    return nullptr;
}
