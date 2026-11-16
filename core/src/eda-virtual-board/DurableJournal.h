#pragma once
#include <eda/api/Types.h>
#include "LiveTraceStore.h"
#include "eda-platform/DurableFile.h"
#include <filesystem>
namespace eda::virtual_board {
struct JournalDigests {std::string circuit,control;};
inline constexpr const char* JournalDigestVersion="sigflow.sha256-chain.v2";
struct JournalRecovery {Json records=Json::array();uint64_t valid_bytes=0,truncated_bytes=0;};
class DurableJournal {
public:
    DurableJournal(std::string durability,const std::filesystem::path& path={},size_t max_bytes=128*1024*1024);
    void AppendRecord(const Json&);
    void BeginTransaction(const std::string& batch_id,const EventPosition&,const Json& effective_inputs);
    void Prepare(const Json& transaction);
    void MarkCommitted(const TraceCommit&);
    void Close();
    const Json& Records()const{return records_;}
    const std::string& Durability()const{return durability_;}
    const JournalDigests& CurrentDigests()const{return digests_;}
    static JournalRecovery Recover(const std::filesystem::path&,bool repair_tail=true);
    static JournalDigests Digests(const Json&);
    static std::shared_ptr<LiveTraceStore> RestoreTrace(const Json&);
private:
    void WriteEnvelope(const Json&,bool sync);
    std::string durability_,previous_=std::string(64,'0');
    size_t max_bytes_,bytes_=0;
    uint64_t sequence_=0;
    bool closed_=false;
    std::unique_ptr<eda::platform::DurableFile> file_;
    Json records_=Json::array(),pending_,begin_;
    std::string prepared_checksum_;
    JournalDigests digests_;
};
Json TraceEventJson(const LiveTransition&);
LiveTransition TraceEventFromJson(const Json&);
}
