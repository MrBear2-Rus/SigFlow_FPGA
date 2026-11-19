#pragma once
#include <eda/api/Types.h>
#include <filesystem>
namespace sigflow::virtual_lab {
eda::Json ExportVirtualCapture(const eda::Json&,const std::filesystem::path&);
}
