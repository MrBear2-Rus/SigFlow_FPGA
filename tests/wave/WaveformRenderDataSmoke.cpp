// 纯数据测试：BuildWaveformFrame 与视口状态变换（无 wx 依赖）。
#include "../../main/trace/TraceCache.h"
#include "../../main/trace/VcdLazyTraceSource.h"
#include "../../main/wave/WaveformRenderData.h"
#include "../../main/wave/WaveViewState.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

using namespace sigflow::trace;
using namespace sigflow::wave;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& message)
{
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

std::string Bin(unsigned value, unsigned width)
{
    std::string result(width, '0');
    for (unsigned i = 0; i < width; ++i) {
        result[width - 1 - i] = (value & (1u << i)) ? '1' : '0';
    }
    return result;
}

std::string BuildVcd(TimeValue maxTime)
{
    std::ostringstream out;
    out << "$timescale 1ns $end\n";
    out << "$scope module TOP $end\n";
    out << "$var wire 1 ! clk $end\n";
    out << "$var reg 4 \" cnt $end\n";
    out << "$var wire 8 # data $end\n";
    out << "$upscope $end\n";
    out << "$enddefinitions\n";
    out << "$dumpvars\n0!\nb0000 \"\nb00000000 #\n$end\n";
    for (TimeValue t = 0; t <= maxTime; t += 5) {
        out << "#" << t << "\n";
        out << (((t / 5) % 2) ? "1!" : "0!") << "\n";
        if (t % 10 == 0) {
            out << "b" << Bin(static_cast<unsigned>((t / 10) % 16), 4) << " \"\n";
            out << "b" << Bin(static_cast<unsigned>((t / 10) % 256), 8) << " #\n";
        }
    }
    return out.str();
}

void CheckFrame(TraceSource& source, const WaveViewState& state, int width,
                const std::string& label)
{
    WaveformFrame frame;
    Check(BuildWaveformFrame(source, state, width, 300, frame), label + " build");
    Check(frame.signals.size() == 3, label + " signal count");
    for (const WaveformSignalData& signal : frame.signals) {
        Check(!signal.polyline.empty(), label + " polyline non-empty: " + signal.name);
    }
    Check(!frame.ticks.empty(), label + " ticks non-empty");
}

} // namespace

int main()
{
    const std::string path =
        (std::filesystem::temp_directory_path() / "sigflow_renderdata_smoke.vcd").string();
    {
        std::ofstream out(path, std::ios::trunc);
        out << BuildVcd(50000);
    }

    auto inner = std::make_unique<VcdLazyTraceSource>();
    auto source = std::make_shared<CachingTraceSource>(std::move(inner), 4 * 1024 * 1024);
    std::string error;
    Check(source->Open(path, error), "open: " + error);

    WaveViewState state;
    state.maxTime = source->TimeRange().end;
    state.valid = true;
    for (const SignalInfo& signal : source->Signals()) {
        state.visibleSignalIds.push_back(signal.id);
    }

    // 多个窗口
    state.timeOffset = 0;
    state.timeSpan = 1000;
    CheckFrame(*source, state, 800, "w1");

    state.timeOffset = 1000;
    state.timeSpan = 1000;
    CheckFrame(*source, state, 800, "w2");

    state.timeOffset = 25000;
    state.timeSpan = 1000;
    CheckFrame(*source, state, 800, "w3");

    state.timeOffset = 0;
    state.timeSpan = 50001;
    CheckFrame(*source, state, 800, "full");

    // 视口状态变换
    WaveViewInteraction::ZoomAt(state, 0.5, 0.7);
    Check(state.timeSpan < 50001, "zoom reduces span");
    WaveViewInteraction::PanBy(state, 0.2);
    Check(state.valid, "pan keeps valid");
    WaveViewInteraction::JumpToTime(state, 30000);
    CheckFrame(*source, state, 800, "after-jump");
    WaveViewInteraction::Reset(state);
    Check(state.timeOffset == 0, "reset offset");

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(TraceSidecarIndex::SidecarPathFor(path), ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cout << g_failures << " FAILURES\n";
    return 1;
}
