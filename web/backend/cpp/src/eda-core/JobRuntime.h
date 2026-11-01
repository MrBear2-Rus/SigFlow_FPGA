#pragma once
#include "JobProtocol.h"
#include <functional>
#include <eda/api/toolchain.hpp>
namespace eda::web {
// Access settings without adding Web-specific fields to the frozen JobContext API.
const ExecutionRequest& ExecutionFor(const JobContext& context);
std::filesystem::path CheckedInput(const JobContext& context, const std::filesystem::path& path);
ToolResolution ResolveExecutionTool(const JobContext& context, ToolQuery query);
using EventSink = std::function<void(const Json&)>;
// Throws before/when persistence fails. No success result is emitted in that case.
int ExecuteJob(ExecutionRequest input, IJobProvider& provider, const EventSink& sink,
               const std::function<bool()>& cancelled = {});
}
