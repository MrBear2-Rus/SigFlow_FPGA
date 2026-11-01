#pragma once

#include <string>

#include "abi.h"

namespace eda {

// 返回编译器 id/版本、C++ 标准与 ABI 版本，供 PluginInfo.buildInfo /
// descriptor.build_info 的构建指纹比对使用。
std::string BuildInfo();

} // namespace eda

#define EDA_BUILD_INFO_ABI EDA_PLUGIN_ABI_VERSION
