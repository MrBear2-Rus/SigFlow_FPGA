#pragma once
#include "VirtualSession.h"
namespace eda::virtual_board {
// Versioned P1 scheduler shares the validated PadMap, resolver, DUT factory,
// LiveTrace and durable transaction primitives with the M0 session.
class Phase1Session {
public:
    Phase1Session(SessionContract,VirtualSession::Factory);
    ~Phase1Session();
    Json Submit(Json);
    std::shared_ptr<const LiveTraceStore> Trace()const;
    static Json Replay(const SessionContract&,VirtualSession::Factory,const Json&);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
