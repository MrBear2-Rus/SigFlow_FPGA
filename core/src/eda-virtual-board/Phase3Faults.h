#pragma once
#include "Phase1Faults.h"
namespace eda::virtual_board {
// Phase 3 teaching timing faults (planning book §3.1):
//  - reset_glitch: exact-width pulse injected on an external reset contact.
//    The narrow pulse is scheduled through the same edge/expansion pipeline
//    as button bounce, so it reaches eval even while the clock is stopped.
//  - clock_divider_offset: divide-ratio offset on the rational main clock;
//    the scheduler restarts the half-period accumulator and records the
//    rescheduling decision in the deterministic fault event ledger.
bool ValidatePhase3Fault(const BoardTopology&,const Json& rule);
}
