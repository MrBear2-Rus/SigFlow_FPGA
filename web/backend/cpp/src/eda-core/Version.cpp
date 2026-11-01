#include <eda/api/build_info.h>

#include "Platform.h"

#include <sstream>

namespace eda {

std::string BuildInfo() {
    std::ostringstream os;
    os << "compiler=" << platform::CompilerInfo() << ";cxx=" << __cplusplus
       << ";abi=" << EDA_PLUGIN_ABI_VERSION;
    return os.str();
}

} // namespace eda
