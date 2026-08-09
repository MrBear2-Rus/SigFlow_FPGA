#pragma once

#include "WaveViewState.h"

#include <vector>

namespace sigflow {
namespace wave {

class WaveformView;

// Compare 双窗联动中心（W3-06）：注册视图，广播时间窗与播放头。
class WaveCompareHub {
public:
    static void Register(WaveformView* view);
    static void Unregister(WaveformView* view);
    static void Clear();

    static bool LinkTimeView();
    static void SetLinkTimeView(bool enabled);
    static bool LinkPlayheads();
    static void SetLinkPlayheads(bool enabled);

    // 由视图在交互后调用（时间窗或播放头变化）。
    static void OnViewChanged(WaveformView* source);

private:
    static std::vector<WaveformView*>& Registry();
    static bool s_linkTimeView;
    static bool s_linkPlayheads;
};

} // namespace wave
} // namespace sigflow
