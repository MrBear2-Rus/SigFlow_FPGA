// W1 数据层 smoke 测试：VCD 懒加载、侧车索引、内存预算 LRU、查询缓存。
#include "../../main/trace/TraceCache.h"
#include "../../main/trace/TraceMemoryBudget.h"
#include "../../main/trace/TraceSidecarIndex.h"
#include "../../main/trace/VcdLazyTraceSource.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace sigflow::trace;

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

void TestLazySource(const std::string& path)
{
    VcdLazyTraceSource source;
    std::string error;
    Check(source.Open(path, error), "open: " + error);

    const auto& signals = source.Signals();
    Check(signals.size() == 3, "signal count");
    Check(signals[0].name == "clk" && signals[0].width == 1 &&
              signals[0].kind == SignalKind::Scalar,
          "clk info");
    Check(signals[1].name == "cnt" && signals[1].width == 4 &&
              signals[1].kind == SignalKind::Vector,
          "cnt info");
    Check(signals[2].name == "data" && signals[2].width == 8, "data info");
    Check(signals[0].fullName == "TOP.clk", "fullName: " + signals[0].fullName);
    Check(source.TimeRange().valid && source.TimeRange().end == 200000, "time range end");
    Check(source.Timescale() == "1ns", "timescale");

    const SignalInfo* clk = source.SignalById(signals[0].id);
    const SignalInfo* cnt = source.SignalById(signals[1].id);
    const SignalInfo* data = source.SignalById(signals[2].id);
    Check(clk && cnt && data, "SignalById");

    std::vector<Transition> out;
    Check(source.Query(*clk, 0, 20, out, error), "query clk");
    Check(out.size() == 5 && out[0].time == 0 && out[0].value == "0" &&
              out[4].time == 20 && out[4].value == "0",
          "clk window values");

    Check(source.Query(*cnt, 0, 30, out, error), "query cnt");
    Check(out.size() == 4 && out[0].value == "0000" && out[3].time == 30 &&
              out[3].value == "0011",
          "cnt window values");

    Check(source.Query(*data, 11, 29, out, error), "query data mid");
    Check(out.size() == 1 && out[0].time == 20 && out[0].value == "00000010",
          "data mid window");

    Check(source.Query(*cnt, 11, 19, out, error), "query empty window");
    Check(out.empty(), "empty window");

    std::string value;
    Check(source.ValueAt(*cnt, 25, value, error) && value == "0010", "ValueAt cnt 25");
    Check(source.ValueAt(*cnt, 0, value, error) && value == "0000", "ValueAt cnt 0");
    Check(source.ValueAt(*clk, 7, value, error) && value == "1", "ValueAt clk 7");

    // 大时间窗口：验证时间索引 seek 到文件后部仍然正确
    Check(source.Query(*clk, 150000, 150020, out, error), "query far window");
    Check(out.size() == 5 && out[0].time == 150000 && out[4].time == 150020,
          "far window size");
    Check(out[0].value == "0" && out[1].value == "1", "far window values");
}

void TestSidecar(const std::string& path)
{
    const std::string sidecarPath = TraceSidecarIndex::SidecarPathFor(path);
    {
        VcdLazyTraceSource source;
        std::string error;
        Check(source.Open(path, error), "sidecar first open");
        Check(std::filesystem::exists(sidecarPath), "sidecar file created");
    }
    {
        VcdLazyTraceSource source;
        std::string error;
        Check(source.Open(path, error), "sidecar second open");
        Check(source.HasSidecar(), "sidecar loaded on second open");
        Check(source.TimeRange().end == 200000, "sidecar time range");
        std::vector<Transition> out;
        const SignalInfo& cnt = source.Signals()[1];
        Check(source.Query(cnt, 0, 30, out, error) && out.size() == 4,
              "sidecar query same result");
    }
}

void TestMemoryBudget()
{
    TraceMemoryBudget budget(100);
    Check(budget.Insert("a", 40).empty() && budget.UsedBytes() == 40, "insert a");
    Check(budget.Insert("b", 30).empty() && budget.UsedBytes() == 70, "insert b");
    const std::vector<std::string> evicted = budget.Insert("c", 50);
    Check(evicted.size() == 1 && evicted[0] == "a", "evict a");
    Check(!budget.Contains("a") && budget.Contains("b") && budget.Contains("c"),
          "contains after evict");
    Check(budget.UsedBytes() == 80, "used after evict");

    budget.Touch("b");
    const std::vector<std::string> evicted2 = budget.Insert("d", 60);
    Check(evicted2.size() == 1 && evicted2[0] == "c", "evict c after touch");
    Check(budget.UsedBytes() == 90, "used 90");

    budget.SetMaxBytes(50);
    Check(budget.UsedBytes() <= 50 || budget.EntryCount() == 1, "budget shrink");
    budget.Clear();
    Check(budget.UsedBytes() == 0 && budget.EntryCount() == 0, "budget clear");
}

void TestCache(const std::string& path)
{
    auto inner = std::make_unique<VcdLazyTraceSource>();
    CachingTraceSource cache(std::move(inner), 4096);
    std::string error;
    Check(cache.Open(path, error), "cache open");

    const SignalInfo& clk = cache.Signals()[0];
    std::vector<Transition> out;
    Check(cache.Query(clk, 0, 100, out, error) && out.size() == 21, "cache query first");
    Check(cache.CacheEntries() == 1, "cache entries after first query");
    Check(cache.Query(clk, 0, 100, out, error) && out.size() == 21, "cache query second");
    Check(cache.CacheEntries() == 1, "cache hit keeps one entry");

    std::string value;
    Check(cache.ValueAt(clk, 7, value, error) && value == "1", "cache ValueAt");

    for (TimeValue t = 0; t < 5000; t += 5) {
        std::vector<Transition> window;
        cache.Query(clk, t, t + 5, window, error);
    }
    Check(cache.CacheBytes() <= 4096, "cache bytes within budget");
    Check(cache.CacheEntries() >= 1, "cache entries non-empty");
}

} // namespace

int main()
{
    const std::string path =
        (std::filesystem::temp_directory_path() / "sigflow_trace_lazy_smoke.vcd").string();
    {
        std::ofstream out(path, std::ios::trunc);
        out << BuildVcd(200000);
    }

    TestLazySource(path);
    TestSidecar(path);
    TestMemoryBudget();
    TestCache(path);

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
