#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eda {

struct WaveSignal {
    int id = -1;
    std::string name;      // 叶子名，如 clk
    std::string scope;     // 层级路径，如 TOP
    std::string fullName;  // scope + name
    std::string idCode;    // VCD 标识符
    unsigned width = 1;
};

struct WaveTransition {
    std::uint64_t time = 0;
    std::string value;  // 标量 "0"/"1"/"x"/"z"；向量为二进制串
};

struct WaveTimeRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    bool valid = false;
};

// P1-9：波形数据层抽象（不依赖 UI/wx）。实现须无定容截断（超限显式报错），并自行管理资源（RAII）。
class IWaveformBackend {
public:
    virtual ~IWaveformBackend() = default;

    virtual bool Open(const std::string& path, std::string& error) = 0;
    virtual const std::vector<WaveSignal>& Signals() const = 0;
    virtual std::string Timescale() const = 0;
    virtual WaveTimeRange TimeRange() const = 0;

    // 返回时间在 [t0, t1] 闭区间内的全部跳变。
    virtual bool Query(int signalId, std::uint64_t t0, std::uint64_t t1,
                       std::vector<WaveTransition>& out, std::string& error) = 0;
    // 返回 t 时刻的信号值（最后一个不晚于 t 的跳变；无跳变时为空）。
    virtual bool ValueAt(int signalId, std::uint64_t t, std::string& value,
                         std::string& error) = 0;
};

} // namespace eda
