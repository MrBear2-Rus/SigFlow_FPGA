#include "Logger.h"

#include <utility>

namespace eda {
namespace {
LogSink& Sink() {
    static LogSink sink;
    return sink;
}
} // namespace

void SetLogSink(LogSink sink) { Sink() = std::move(sink); }

void Log(LogLevel level, const std::string& message) {
    if (Sink()) {
        Sink()(level, message);
    }
}

} // namespace eda
