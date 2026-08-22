#pragma once

#include "TraceTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace sigflow {
namespace trace {

// 统一 trace 加载接口（W1-01）。数据层不依赖任何 UI / wx 类型。
class TraceSource {
public:
    virtual ~TraceSource() = default;

    virtual bool Open(const std::string& path, std::string& error) = 0;

    virtual const std::vector<SignalInfo>& Signals() const = 0;
    virtual const SignalInfo* SignalById(int id) const = 0;
    virtual TraceTimeRange TimeRange() const = 0;
    virtual std::string Timescale() const = 0;
    virtual const std::string& Path() const = 0;

    // 返回时间在 [t0, t1] 闭区间内的全部跳变（不含 t0 之前的初始值）。
    virtual bool Query(const SignalInfo& signal, TimeValue t0, TimeValue t1,
                       std::vector<Transition>& out, std::string& error) = 0;

    // 返回 t 时刻的信号值（最后一个不晚于 t 的跳变；无跳变时为 "x"）。
    virtual bool ValueAt(const SignalInfo& signal, TimeValue t,
                         std::string& value, std::string& error) = 0;

    virtual bool HasSidecar() const = 0;
};

} // namespace trace
} // namespace sigflow
