#pragma once

#include <functional>
#include <string>

namespace eda {

enum class LogLevel { Debug, Info, Warning, Error };

using LogSink = std::function<void(LogLevel, const std::string&)>;

// 进程内日志下沉点；未设置时静默丢弃。宿主/测试可注入自己的 sink。
void SetLogSink(LogSink sink);
void Log(LogLevel level, const std::string& message);

} // namespace eda
