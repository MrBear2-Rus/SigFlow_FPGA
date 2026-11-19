#pragma once
#include <eda/api/Types.h>
namespace eda::virtual_board {
Json StudentProjection(const Json&,const std::string& runToken);
bool StudentCommandAllowed(const std::string& operation);
}
