#include "plugins/pnr-nextpnr/NextpnrAnalysis.h"
#include "plugins/pnr-nextpnr/NextpnrLogModel.h"
#include <stdexcept>
#include <string>
#include <iostream>

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    try {
        using namespace eda::pnr;
        auto empty = AnalyzeNextpnrLog("", false, "device", "family", -1);
        Check(!empty.at("parsed").get<bool>(), "empty log must not be parsed");
        Check(!empty.at("log_truncated").get<bool>(), "empty log is not truncated");
        auto bytes =
            AnalyzeNextpnrLog(std::string(4 * 1024 * 1024 + 1, 'x'), false, "device", "family", 0);
        Check(bytes.at("parsed").get<bool>() && bytes.at("log_truncated").get<bool>(),
              "byte limit must mark incomplete analysis");
        auto lines = AnalyzeNextpnrLog(std::string(50001, '\n'), false, "device", "family", 0);
        Check(lines.at("parsed").get<bool>() && lines.at("log_truncated").get<bool>(),
              "line count limit must mark incomplete analysis");
        const auto counts = AnalyzeNextpnrLog(
            "Warning: first\nInfo: 3 warnings, 2 errors\nWarning: error output unavailable\n",
            false, "device", "family", 0);
        Check(counts.at("warnings") == 3 && counts.at("errors") == 2,
              "later unrelated warning must not hide final counts");
        NextpnrLogParser parser;
        NextpnrRunRecord record;
        Check(parser.Parse("Warning: one\n", "", record) && record.warningCount == 1,
              "first parse warning count");
        Check(parser.Parse("Info: clean\n", "", record) && record.warningCount == 0,
              "reused record must reset parse counters");
        std::cout << "nextpnr bounded analysis checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
