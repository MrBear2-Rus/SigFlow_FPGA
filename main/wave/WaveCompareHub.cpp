#include "WaveCompareHub.h"

#include "WaveformView.h"

#include <algorithm>

namespace sigflow {
namespace wave {

bool WaveCompareHub::s_linkTimeView = true;
bool WaveCompareHub::s_linkPlayheads = true;

std::vector<WaveformView*>& WaveCompareHub::Registry()
{
    static std::vector<WaveformView*> registry;
    return registry;
}

void WaveCompareHub::Register(WaveformView* view)
{
    std::vector<WaveformView*>& registry = Registry();
    if (std::find(registry.begin(), registry.end(), view) == registry.end()) {
        registry.push_back(view);
    }
}

void WaveCompareHub::Unregister(WaveformView* view)
{
    std::vector<WaveformView*>& registry = Registry();
    registry.erase(std::remove(registry.begin(), registry.end(), view), registry.end());
}

void WaveCompareHub::Clear()
{
    Registry().clear();
}

bool WaveCompareHub::LinkTimeView()
{
    return s_linkTimeView;
}

void WaveCompareHub::SetLinkTimeView(bool enabled)
{
    s_linkTimeView = enabled;
}

bool WaveCompareHub::LinkPlayheads()
{
    return s_linkPlayheads;
}

void WaveCompareHub::SetLinkPlayheads(bool enabled)
{
    s_linkPlayheads = enabled;
}

void WaveCompareHub::OnViewChanged(WaveformView* source)
{
    for (WaveformView* view : Registry()) {
        if (view == source || !view) continue;
        if (s_linkTimeView) view->ApplyLinkedWindow(source);
        if (s_linkPlayheads) view->ApplyLinkedPlayhead(source);
    }
}

} // namespace wave
} // namespace sigflow
