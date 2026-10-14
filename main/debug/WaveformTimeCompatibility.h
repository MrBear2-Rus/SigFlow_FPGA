#pragma once
#include "../trace/TraceTimebase.h"
#include <limits>
#include <string>
extern "C" {
#include "vcd.h"
}

namespace sigflow::debug {
inline bool CompatibleWaveformTime(vcd_t* hw, vcd_t* sim, std::string& error)
{
    if (!hw || !sim) { error = "VCD data is missing"; return false; }
    if (hw->sample_index_time || sim->sample_index_time) {
        error = "TIME_DOMAIN_UNKNOWN: sample-index captures require physical time metadata";
        return false;
    }
    std::uint64_t hwTick = 0, simTick = 0;
    if (!sigflow::trace::VcdTickFemtoseconds(std::to_string(hw->timescale.scale) + hw->timescale.unit, hwTick) ||
        !sigflow::trace::VcdTickFemtoseconds(std::to_string(sim->timescale.scale) + sim->timescale.unit, simTick)) {
        error = "TIME_DOMAIN_UNKNOWN: missing or invalid VCD timescale"; return false;
    }
    if (hwTick != simTick) {
        error = "TIMEBASE_MISMATCH: normalize VCD tick units before comparison"; return false;
    }
    // Offset inference subtracts signed times and compares offset differences.
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 2);
    if (vcd_get_max_timestamp(hw) > limit || vcd_get_max_timestamp(sim) > limit ||
        (hw->has_trigger_time && hw->trigger_time > limit) ||
        (sim->has_trigger_time && sim->trigger_time > limit)) {
        error = "TIME_RANGE_UNSUPPORTED: signed comparison range exceeded"; return false;
    }
    return true;
}
} // namespace sigflow::debug
