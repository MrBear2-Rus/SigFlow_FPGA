#include "TraceTypes.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace sigflow {
namespace trace {

bool TraceCoverageInfo::FindGap(TimeValue begin, TimeValue end,
                                TraceCoverageRange& gap) const
{
    gap = TraceCoverageRange{};
    if (!known || end < begin) return false;

    std::vector<TraceCoverageRange> ranges;
    ranges.reserve(covered.size());
    for (const TraceCoverageRange& range : covered) {
        if (range.end < range.begin || range.end < begin || range.begin > end) continue;
        TraceCoverageRange clipped = range;
        clipped.begin = std::max(clipped.begin, begin);
        clipped.end = std::min(clipped.end, end);
        ranges.push_back(std::move(clipped));
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const TraceCoverageRange& lhs, const TraceCoverageRange& rhs) {
                  if (lhs.begin != rhs.begin) return lhs.begin < rhs.begin;
                  return lhs.end < rhs.end;
              });

    TimeValue cursor = begin;
    for (const TraceCoverageRange& range : ranges) {
        if (range.begin > cursor) {
            gap.begin = cursor;
            gap.end = range.begin - 1;
            gap.reason = "trace coverage gap";
            return true;
        }
        if (range.end >= cursor) {
            if (range.end == std::numeric_limits<TimeValue>::max()) {
                return false;
            }
            cursor = std::max(cursor, range.end + 1);
        }
        if (cursor > end) return false;
    }
    if (cursor <= end) {
        gap.begin = cursor;
        gap.end = end;
        gap.reason = "trace coverage gap";
        return true;
    }
    return false;
}

} // namespace trace
} // namespace sigflow
