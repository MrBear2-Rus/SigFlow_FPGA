#pragma once
#include "JobRuntime.h"
#include <memory>
namespace eda::web {
using ProviderFactory = std::function<std::unique_ptr<IJobProvider>(const std::string&)>;
int RunCommand(int argc, char** argv, const ProviderFactory& factory, const EventSink& sink,
               const std::function<bool()>& cancelled, const std::string& defaultDataDir,
               const std::string& defaultToolRoot);
}  // namespace eda::web
