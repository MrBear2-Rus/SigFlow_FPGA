// W3 纯数据测试：A-B 测量统计与多信号模式搜索。
#include "../../main/trace/TraceCache.h"
#include "../../main/trace/VcdLazyTraceSource.h"
#include "../../main/wave/WaveAnalysis.h"
#include "../../main/wave/WavePatternSearch.h"

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

} // namespace

int main()
{
    const std::string path =
        (std::filesystem::temp_directory_path() / "sigflow_analysis_smoke.vcd").string();
    {
        std::ofstream out(path, std::ios::trunc);
        out << BuildVcd(50000);
    }

    auto inner = std::make_unique<VcdLazyTraceSource>();
    auto source = std::make_shared<CachingTraceSource>(std::move(inner), 4 * 1024 * 1024);
    std::string error;
    Check(source->Open(path, error), "open: " + error);

    const auto& signals = source->Signals();
    const SignalInfo& clk = signals[0];
    const SignalInfo& cnt = signals[1];
    const SignalInfo& data = signals[2];

    // ── 测量：clk 在 [0, 100] ──
    WaveMeasurement measure;
    Check(MeasureSignal(*source, clk, 0, 100, 1, measure), "measure clk");
    Check(measure.valid, "measure valid");
    // clk 每 5ns 跳变，[0,100] 含两端共 21 次跳变
    Check(measure.edgeCount == 21, "clk edge count: " + std::to_string(measure.edgeCount));
    Check(measure.xzCount == 0, "clk xz count");
    // 上升沿每 10ns，avgPeriod ≈ 10
    Check(measure.avgPeriod > 9.5 && measure.avgPeriod < 10.5,
          "clk avg period: " + std::to_string(measure.avgPeriod));
    Check(measure.dutyCycle > 0.45 && measure.dutyCycle < 0.55,
          "clk duty: " + std::to_string(measure.dutyCycle));

    // ── 测量：cnt 在 [0, 30]（4 次跳变，周期 10，占空比按高电平段计）──
    Check(MeasureSignal(*source, cnt, 0, 30, 1, measure), "measure cnt");
    Check(measure.edgeCount == 4, "cnt edge count: " + std::to_string(measure.edgeCount));

    // ── 模式搜索 ──
    WavePatternSpec risingClk;
    risingClk.criteria.push_back({ clk.id, WavePatternKind::RisingEdge, "" });
    TimeValue found = 0;
    Check(WavePatternFind(*source, risingClk, 0, 100, true, 1, found, error) && found == 5,
          "first rising edge should be 5, got " + std::to_string(found));
    Check(WavePatternFind(*source, risingClk, 0, 100, true, 2, found, error) && found == 15,
          "second rising edge should be 15, got " + std::to_string(found));
    Check(WavePatternFind(*source, risingClk, 0, 100, false, 1, found, error) && found == 95,
          "last rising edge should be 95, got " + std::to_string(found));

    WavePatternSpec clkHighCntValue;
    clkHighCntValue.criteria.push_back({ clk.id, WavePatternKind::High, "" });
    clkHighCntValue.criteria.push_back({ cnt.id, WavePatternKind::Value, "0010" });
    std::vector<TimeValue> matches =
        WavePatternCollect(*source, clkHighCntValue, 0, 100, 0, error);
    // cnt==0010 的区间为 [20,30)，clk 高电平时刻 25
    Check(!matches.empty() && matches.front() == 25,
          "combined match first should be 25, got " +
              (matches.empty() ? std::string("none") : std::to_string(matches.front())));

    WavePatternSpec anyEdge;
    anyEdge.criteria.push_back({ cnt.id, WavePatternKind::AnyEdge, "" });
    Check(WavePatternFind(*source, anyEdge, 5, 25, true, 1, found, error) && found == 10,
          "cnt any edge first should be 10, got " + std::to_string(found));

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
