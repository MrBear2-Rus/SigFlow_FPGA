#include "ReplayScenario.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <cstdlib>

namespace sigflow {
namespace debug {
namespace {

std::string NormalizeValue(const char* raw) {
    if (!raw) return {};
    std::string value(raw);
    if (!value.empty() && (value.front() == 'b' || value.front() == 'B')) {
        value.erase(value.begin());
    }
    return value;
}

std::string LeafName(const std::string& name) {
    const std::size_t slash = name.find_last_of("./");
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

signal_t* FindSignal(vcd_t* vcd, const std::string& name) {
    if (!vcd || name.empty()) return nullptr;
    for (std::size_t i = 0; i < vcd->signals_count; ++i) {
        signal_t* signal = &vcd->signals[i];
        if (name == signal->name || name == signal->full_name) return signal;
    }
    const std::string leaf = LeafName(name);
    for (std::size_t i = 0; i < vcd->signals_count; ++i) {
        signal_t* signal = &vcd->signals[i];
        if (leaf == signal->name || leaf == LeafName(signal->full_name)) return signal;
    }
    return nullptr;
}

bool IsIdentifier(const std::string& value) {
    if (value.empty()) return false;
    if (!(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_')) {
        return false;
    }
    for (const char c : value) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

bool ParseValue(const std::string& raw, std::uint64_t& value) {
    std::string bits = NormalizeValue(raw.c_str());
    if (bits.empty()) return false;
    if (bits.size() > 64) return false;
    value = 0;
    for (const char c : bits) {
        if (c != '0' && c != '1') return false;
        value = (value << 1) | static_cast<std::uint64_t>(c - '0');
    }
    return true;
}

std::string ValueLiteral(const std::string& raw) {
    std::uint64_t value = 0;
    if (!ParseValue(raw, value)) return {};
    std::ostringstream output;
    output << "0x" << std::hex << value << "ULL";
    return output.str();
}

std::string EscapeString(const std::string& value) {
    std::string escaped;
    for (const char c : value) {
        if (c == '\\' || c == '"') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

std::uint64_t InferHalfPeriod(signal_t* clock) {
    if (!clock || clock->changes_count < 2) return 0;
    std::uint64_t best = 0;
    for (std::size_t i = 1; i < clock->changes_count; ++i) {
        const std::uint64_t delta = clock->value_changes[i].timestamp -
                                    clock->value_changes[i - 1].timestamp;
        if (delta != 0 && (best == 0 || delta < best)) best = delta;
    }
    return best;
}

} // namespace

bool ReplayStimulusExtractor::Extract(vcd_t* capture,
                                       const std::vector<std::string>& inputSignals,
                                       const std::string& clockSignal,
                                       ReplayScenario& scenario,
                                       std::string& error) {
    scenario = ReplayScenario();
    if (!capture) {
        error = "capture VCD is empty";
        return false;
    }
    if (inputSignals.empty()) {
        error = "at least one replay input is required";
        return false;
    }
    signal_t* clock = FindSignal(capture, clockSignal);
    if (!clock) {
        error = "replay clock signal not found: " + clockSignal;
        return false;
    }
    scenario.clockSignal = LeafName(clock->full_name[0] ? clock->full_name : clock->name);
    scenario.clockHalfPeriod = InferHalfPeriod(clock);
    if (clock->changes_count > 0) {
        scenario.initialClockValue = NormalizeValue(clock->value_changes[0].value);
    }

    std::map<std::pair<std::uint64_t, std::string>, std::string> events;
    for (const std::string& requested : inputSignals) {
        signal_t* signal = FindSignal(capture, requested);
        if (!signal) continue;
        ReplaySignal replaySignal;
        replaySignal.sourceName = signal->full_name[0] ? signal->full_name : signal->name;
        replaySignal.portName = LeafName(replaySignal.sourceName);
        replaySignal.width = static_cast<unsigned>(signal->size == 0 ? 1 : signal->size);
        if (!IsIdentifier(replaySignal.portName)) {
            error = "replay input is not a top-level identifier: " + replaySignal.portName;
            return false;
        }
        scenario.inputSignals.push_back(replaySignal);
        for (std::size_t i = 0; i < signal->changes_count; ++i) {
            const auto& change = signal->value_changes[i];
            const std::string value = NormalizeValue(change.value);
            if (value.empty()) continue;
            events[{change.timestamp, replaySignal.portName}] = value;
            scenario.maxTime = std::max<std::uint64_t>(scenario.maxTime, change.timestamp);
        }
    }
    if (scenario.inputSignals.empty()) {
        error = "none of the requested replay inputs exist in capture VCD";
        return false;
    }
    for (const auto& item : events) {
        scenario.assignments.push_back({item.first.first, item.first.second, item.second});
    }
    scenario.maxTime = std::max<std::uint64_t>(scenario.maxTime, vcd_get_max_timestamp(capture));
    return true;
}

ReplayConsistencyReport EvaluateReplayConsistency(const ReplayScenario& scenario,
                                                  std::size_t requestedInputCount) {
    ReplayConsistencyReport report;
    report.requestedInputs = requestedInputCount;
    report.capturedInputs = scenario.inputSignals.size();
    report.assignmentCount = scenario.assignments.size();
    if (requestedInputCount == 0) {
        report.message = "no requested replay inputs";
        return report;
    }
    const double coverage = std::min(1.0,
        static_cast<double>(report.capturedInputs) / static_cast<double>(requestedInputCount));
    const double activity = report.assignmentCount == 0 ? 0.0 : 1.0;
    const double clock = scenario.clockHalfPeriod == 0 ? 0.0 : 1.0;
    report.score = coverage * 0.7 + activity * 0.2 + clock * 0.1;
    report.verified = coverage >= 1.0 && activity > 0.0 && clock > 0.0;
    if (report.verified) report.score = 1.0;
    report.message = report.verified ? "all replay inputs are covered" :
        "replay stimulus coverage or clock inference is incomplete";
    return report;
}

bool ReplayTestbenchGenerator::Generate(const ReplayScenario& scenario,
                                        const std::string& topModule,
                                        const std::string& outputPath,
                                        const std::string& vcdPath,
                                        std::string& error) {
    if (!IsIdentifier(topModule)) {
        error = "invalid replay top module: " + topModule;
        return false;
    }
    if (scenario.inputSignals.empty() || scenario.clockSignal.empty()) {
        error = "replay scenario has no clock or inputs";
        return false;
    }
    if (!IsIdentifier(scenario.clockSignal)) {
        error = "invalid replay clock signal: " + scenario.clockSignal;
        return false;
    }
    std::ofstream output(outputPath, std::ios::trunc);
    if (!output) {
        error = "cannot create replay testbench: " + outputPath;
        return false;
    }
    output << "#include \"V" << topModule << ".h\"\n"
           << "#include \"verilated.h\"\n"
           << "#include \"verilated_vcd_c.h\"\n"
           << "#include <algorithm>\n"
           << "#include <cstdint>\n\n"
           << "int main(int argc, char** argv) {\n"
           << "    Verilated::commandArgs(argc, argv);\n"
           << "    Verilated::traceEverOn(true);\n"
           << "    V" << topModule << "* top = new V" << topModule << ";\n"
           << "    VerilatedVcdC* trace = new VerilatedVcdC;\n"
           << "    top->trace(trace, 99);\n"
           << "    trace->open(\"" << EscapeString(vcdPath) << "\");\n"
           << "    vluint64_t simTime = 0;\n"
           << "    top->" << scenario.clockSignal << " = "
           << (ValueLiteral(scenario.initialClockValue).empty() ? "0ULL" : ValueLiteral(scenario.initialClockValue)) << ";\n"
           << "    top->eval();\n"
           << "    trace->dump(simTime);\n";

    std::map<std::uint64_t, std::vector<const ReplayAssignment*>> grouped;
    for (const auto& assignment : scenario.assignments) grouped[assignment.time].push_back(&assignment);
    std::uint64_t lastTime = 0;
    for (const auto& item : grouped) {
        if (item.first < lastTime) continue;
        output << "    while (simTime < " << item.first << "ULL) {\n"
               << "        ++simTime;\n";
        if (scenario.clockHalfPeriod > 0) {
            output << "        if ((simTime % " << scenario.clockHalfPeriod << "ULL) == 0) top->"
                   << scenario.clockSignal << " = !top->" << scenario.clockSignal << ";\n";
        }
        output << "        top->eval();\n"
               << "        trace->dump(simTime);\n"
               << "    }\n";
        for (const ReplayAssignment* assignment : item.second) {
            const std::string literal = ValueLiteral(assignment->value);
            if (literal.empty()) {
                error = "replay value contains X/Z or exceeds 64 bits: " + assignment->signalName;
                return false;
            }
            output << "    top->" << assignment->signalName << " = " << literal << ";\n";
        }
        output << "    top->eval();\n"
               << "    trace->dump(simTime);\n";
        lastTime = item.first;
    }
    const std::uint64_t extra = std::max<std::uint64_t>(100, scenario.clockHalfPeriod * 10);
    output << "    for (vluint64_t i = 0; i < " << extra << "ULL; ++i) {\n"
           << "        ++simTime;\n";
    if (scenario.clockHalfPeriod > 0) {
        output << "        if ((simTime % " << scenario.clockHalfPeriod << "ULL) == 0) top->"
               << scenario.clockSignal << " = !top->" << scenario.clockSignal << ";\n";
    }
    output << "        top->eval();\n"
           << "        trace->dump(simTime);\n"
           << "    }\n"
           << "    trace->close();\n"
           << "    delete trace;\n"
           << "    delete top;\n"
           << "    return 0;\n"
           << "}\n";
    return output.good();
}

bool ReplayRunner::Run(const std::string& command,
                       const std::string& workingDirectory,
                       ReplayRunResult& result,
                       std::string& error) {
    result = ReplayRunResult();
    if (command.empty()) {
        error = "replay command is empty";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path previous = std::filesystem::current_path(ec);
    if (!workingDirectory.empty()) {
        std::filesystem::current_path(workingDirectory, ec);
        if (ec) {
            error = "cannot enter replay working directory: " + workingDirectory;
            return false;
        }
    }
    result.exitCode = std::system(command.c_str());
    if (!workingDirectory.empty()) std::filesystem::current_path(previous, ec);
    result.success = result.exitCode == 0;
    result.message = result.success ? "replay completed" : "replay command failed";
    if (!result.success) error = result.message;
    return result.success;
}

} // namespace debug
} // namespace sigflow
