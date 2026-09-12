#include "DebugBehaviorSummary.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace sigflow {
namespace debug {

namespace {

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool Contains(const std::string& value, const char* token)
{
    return Lower(value).find(token) != std::string::npos;
}

std::string SignalName(const signal_t& signal)
{
    return signal.full_name[0] ? signal.full_name : signal.name;
}

signal_t* FindSignal(vcd_t* vcd, const std::string& hint)
{
    if (!vcd || hint.empty()) return nullptr;
    for (std::size_t i = 0; i < vcd->signals_count; ++i) {
        signal_t* signal = &vcd->signals[i];
        if (SignalName(*signal) == hint || signal->name == hint) return signal;
    }
    for (std::size_t i = 0; i < vcd->signals_count; ++i) {
        signal_t* signal = &vcd->signals[i];
        if (Contains(SignalName(*signal), hint.c_str()) || Contains(signal->name, hint.c_str())) {
            return signal;
        }
    }
    return nullptr;
}

signal_t* FindByTokens(vcd_t* vcd, const char* first, const char* second = nullptr)
{
    if (!vcd) return nullptr;
    for (std::size_t i = 0; i < vcd->signals_count; ++i) {
        signal_t* signal = &vcd->signals[i];
        const std::string name = Lower(SignalName(*signal));
        if (name.find(first) != std::string::npos &&
            (!second || name.find(second) != std::string::npos)) return signal;
    }
    return nullptr;
}

bool IsHigh(const char* value)
{
    return value && (value[0] == '1' || value[0] == 'h' || value[0] == 'H');
}

bool IsLow(const char* value)
{
    return value && (value[0] == '0' || value[0] == 'l' || value[0] == 'L');
}

const char* ValueAt(signal_t* signal, timestamp_t time)
{
    if (!signal || signal->changes_count == 0) return nullptr;
    const value_change_t* selected = &signal->value_changes[0];
    for (std::size_t i = 0; i < signal->changes_count; ++i) {
        if (signal->value_changes[i].timestamp > time) break;
        selected = &signal->value_changes[i];
    }
    return selected->value;
}

void AddEvent(DebugBehaviorSummary& summary, const char* kind, timestamp_t time,
              const signal_t* signal, const char* value, const std::string& message)
{
    DebugBehaviorEvent event;
    event.kind = kind;
    event.time = time;
    event.signalName = signal ? SignalName(*signal) : std::string();
    event.value = value ? value : std::string();
    event.message = message;
    summary.events.push_back(std::move(event));
}

void AddResetEvent(DebugBehaviorSummary& summary, vcd_t* capture,
                   const DebugContract& contract)
{
    signal_t* reset = nullptr;
    for (const DebugProbe& probe : contract.probes) {
        if (Contains(probe.path, "rst") || Contains(probe.path, "reset")) {
            reset = FindSignal(capture, probe.path);
            if (reset) break;
        }
    }
    if (!reset) reset = FindByTokens(capture, "rst");
    if (!reset) reset = FindByTokens(capture, "reset");
    if (!reset) return;
    for (std::size_t i = 1; i < reset->changes_count; ++i) {
        const char* before = reset->value_changes[i - 1].value;
        const char* after = reset->value_changes[i].value;
        if (IsLow(before) && IsHigh(after)) {
            AddEvent(summary, "reset_release", reset->value_changes[i].timestamp,
                     reset, after, "reset released");
            return;
        }
    }
}

void AddStateEvent(DebugBehaviorSummary& summary, vcd_t* capture,
                   const DebugContract& contract)
{
    signal_t* state = nullptr;
    for (const DebugProbe& probe : contract.probes) {
        if (Contains(probe.path, "state") || Contains(probe.path, "status")) {
            state = FindSignal(capture, probe.path);
            if (state) break;
        }
    }
    if (!state) state = FindByTokens(capture, "state");
    if (!state) state = FindByTokens(capture, "status");
    if (!state || state->changes_count < 2) return;
    const value_change_t& change = state->value_changes[1];
    AddEvent(summary, "state_change", change.timestamp, state, change.value,
             "state/status changed");
}

void AddHandshakeEvent(DebugBehaviorSummary& summary, vcd_t* capture,
                       const DebugContract& contract)
{
    signal_t* valid = contract.trigger.hsValidPath.empty()
        ? FindByTokens(capture, "valid") : FindSignal(capture, contract.trigger.hsValidPath);
    signal_t* ready = contract.trigger.hsReadyPath.empty()
        ? FindByTokens(capture, "ready") : FindSignal(capture, contract.trigger.hsReadyPath);
    if (!valid || !ready) return;
    for (std::size_t i = 0; i < valid->changes_count; ++i) {
        const value_change_t& change = valid->value_changes[i];
        if (IsHigh(change.value) && IsLow(ValueAt(ready, change.timestamp))) {
            AddEvent(summary, "handshake_wait", change.timestamp, valid, change.value,
                     "valid asserted while ready is low");
            return;
        }
    }
}

Json::Value ToJson(const DebugBehaviorSummary& summary)
{
    Json::Value root(Json::objectValue);
    root["schema_version"] = summary.schemaVersion;
    root["deterministic"] = summary.deterministic;
    root["event_count"] = static_cast<Json::UInt>(summary.events.size());
    Json::Value events(Json::arrayValue);
    for (const DebugBehaviorEvent& event : summary.events) {
        Json::Value item(Json::objectValue);
        item["kind"] = event.kind;
        item["time"] = event.time;
        item["signal"] = event.signalName;
        item["value"] = event.value;
        item["message"] = event.message;
        events.append(item);
    }
    root["events"] = events;
    return root;
}

bool WriteFile(const std::string& path, const std::string& content, std::string& error)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to write behavior summary: " + path;
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        error = "unable to write behavior summary: " + path;
        return false;
    }
    return true;
}

} // namespace

DebugBehaviorSummary DebugBehaviorSummaryBuilder::Extract(
    vcd_t* capture, const DebugContract& contract, const ComparisonResult& comparison)
{
    DebugBehaviorSummary summary;
    if (!capture) return summary;
    AddResetEvent(summary, capture, contract);
    if (comparison.aligned) {
        AddEvent(summary, comparison.alignment.anchorKind == AnchorKind::TriggerHit
                           ? "trigger_hit" : "capture_anchor",
                 comparison.alignment.hwAnchorTime, nullptr, nullptr,
                 comparison.alignment.message);
    }
    AddStateEvent(summary, capture, contract);
    AddHandshakeEvent(summary, capture, contract);
    if (!comparison.firstDiffs.empty()) {
        const WaveformDiff& diff = comparison.firstDiffs.front();
        AddEvent(summary, "first_difference", diff.hwTime, nullptr,
                 diff.actualValue.c_str(), "first hardware/simulation difference");
        summary.events.back().signalName = diff.signalName;
    }
    std::sort(summary.events.begin(), summary.events.end(),
              [](const DebugBehaviorEvent& left, const DebugBehaviorEvent& right) {
                  if (left.time != right.time) return left.time < right.time;
                  return left.kind < right.kind;
              });
    return summary;
}

std::string DebugBehaviorSummaryBuilder::GenerateJson(const DebugBehaviorSummary& summary)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, ToJson(summary)) + "\n";
}

std::string DebugBehaviorSummaryBuilder::GenerateMarkdown(const DebugBehaviorSummary& summary)
{
    std::ostringstream output;
    output << "# TraceBridge 硬件行为摘要\n\n"
           << "- 确定性规则：" << (summary.deterministic ? "是" : "否") << "\n"
           << "- 事件数：" << summary.events.size() << "\n\n"
           << "| 时间 | 类型 | 信号 | 值 | 说明 |\n|---:|---|---|---|---|\n";
    for (const DebugBehaviorEvent& event : summary.events) {
        output << "| " << event.time << " | " << event.kind << " | "
               << event.signalName << " | " << event.value << " | "
               << event.message << " |\n";
    }
    return output.str();
}

bool DebugBehaviorSummaryBuilder::Save(const DebugBehaviorSummary& summary,
                                        const std::string& jsonPath,
                                        const std::string& markdownPath,
                                        std::string& error)
{
    return WriteFile(jsonPath, GenerateJson(summary), error) &&
           WriteFile(markdownPath, GenerateMarkdown(summary), error);
}

} // namespace debug
} // namespace sigflow
