#include "DurableJournal.h"
#include "eda-platform/Sha256.h"
#include <algorithm>
#include <stdexcept>

namespace eda::virtual_board {
namespace {
void Reserve(Json& records){auto& a=records.get_ref<Json::array_t&>();if(a.size()==a.capacity())a.reserve(std::max<size_t>(16,a.capacity()*2));}
std::string Hash(const std::string& s){return platform::Sha256Hex(s.data(),s.size());}
Json Pos(const EventPosition& p){return {{"tick",std::to_string(p.tick)},{"delta",p.delta},{"event_seq",std::to_string(p.event_seq)}};}
uint64_t U64(const Json& j){auto s=j.get<std::string>();if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("E_JOURNAL_NUMBER");size_t n=0;auto v=std::stoull(s,&n);if(n!=s.size())throw std::runtime_error("E_JOURNAL_NUMBER");return v;}
EventPosition Position(const Json& j){return {U64(j.at("tick")),j.at("delta").get<uint32_t>(),U64(j.at("event_seq"))};}
JournalDigests Advance(JournalDigests previous,const Json& record){
    auto kind=record.at("kind").get<std::string>();
    if(kind=="commit"){
        auto inputs=record.at("effective_inputs").get<std::vector<Json>>();
        std::sort(inputs.begin(),inputs.end(),[](const auto& a,const auto& b){return a.at("port_ref").template get<std::string>()<b.at("port_ref").template get<std::string>();});
        Json normalized={{"position",record.at("position")},{"inputs",inputs},{"outputs",record.at("outputs")}};
        if(record.contains("board")){normalized["board"]=record.at("board");normalized["faults"]=record.at("faults");normalized["evaluated"]=record.at("evaluated");}
        previous.circuit=Hash(previous.circuit+"\n"+normalized.dump());
    }else if(kind!="run_header"){
        auto normalized=record;normalized.erase("durability");previous.control=Hash(previous.control+"\n"+normalized.dump());
    }return previous;
}
}
Json TraceEventJson(const LiveTransition& t){return {{"position",Pos(t.position)},{"signal_id",t.signal_id},{"value",t.value},
    {"state_code",t.state_code?Json(*t.state_code):Json(nullptr)},{"valid",t.valid},{"contention",t.contention}};}
LiveTransition TraceEventFromJson(const Json& j){LiveTransition t;t.position=Position(j.at("position"));t.signal_id=j.at("signal_id");t.value=j.at("value");
    if(!j.at("state_code").is_null())t.state_code=j.at("state_code").get<uint8_t>();t.valid=j.at("valid");t.contention=j.at("contention");return t;}
DurableJournal::DurableJournal(std::string mode,const std::filesystem::path& path,size_t maximum):durability_(std::move(mode)),max_bytes_(maximum){
    digests_={Hash("sigflow.circuit.digest.v1"),Hash("sigflow.control.digest.v1")};
    if((durability_!="memory" && durability_!="durable") || !maximum)throw std::runtime_error("E_JOURNAL_CONFIG");
    if(durability_=="durable"){
        if(path.empty())throw std::runtime_error("E_JOURNAL_PATH");
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        file_=std::make_unique<platform::DurableFile>(path,platform::DurableFile::Mode::CreateNew);
    }
}
void DurableJournal::WriteEnvelope(const Json& payload,bool sync){
    if(closed_)throw std::runtime_error("E_JOURNAL_CLOSED");
    Json body={{"seq",std::to_string(sequence_+1)},{"previous",previous_},{"payload",payload}};
    auto checksum=Hash(body.dump());body["sha256"]=checksum;auto line=body.dump()+"\n";
    if(line.size()>max_bytes_-bytes_)throw std::runtime_error("E_JOURNAL_QUOTA");
    if(file_){file_->Append(line);if(sync)file_->Flush();}
    bytes_+=line.size();++sequence_;previous_=checksum;
}
void DurableJournal::AppendRecord(const Json& r){
    if(!pending_.is_null() || !begin_.is_null())throw std::runtime_error("E_JOURNAL_TRANSACTION");
    Json copy=r;auto next=Advance(digests_,r);Reserve(records_);WriteEnvelope({{"type","record"},{"record",r}},true);
    records_.push_back(std::move(copy));digests_=std::move(next);
}
void DurableJournal::BeginTransaction(const std::string& batch,const EventPosition& position,const Json& inputs){
    if(!pending_.is_null() || !begin_.is_null())throw std::runtime_error("E_JOURNAL_TRANSACTION");
    Json start={{"type","begin"},{"batch_id",batch},{"evaluation_position",Pos(position)},{"effective_inputs",inputs}};
    WriteEnvelope(start,false);begin_=std::move(start);
}
void DurableJournal::Prepare(const Json& transaction){
    if(!pending_.is_null() || transaction.at("kind")!="commit")throw std::runtime_error("E_JOURNAL_TRANSACTION");
    if(!begin_.is_null() && (begin_.at("batch_id")!=transaction.at("batch_id") || begin_.at("evaluation_position")!=transaction.at("evaluation_position") ||
       begin_.at("effective_inputs")!=transaction.at("effective_inputs")))throw std::runtime_error("E_JOURNAL_TRANSACTION");
    Json copy=transaction;prepared_checksum_=Hash(copy.dump());
    WriteEnvelope({{"type","prepare"},{"transaction",copy}},false);pending_=std::move(copy);
}
void DurableJournal::MarkCommitted(const TraceCommit& trace){
    if(pending_.is_null() || pending_.at("trace_revision")!=std::to_string(trace.trace_revision) || pending_.at("position")!=Pos(trace.tail_position))
        throw std::runtime_error("E_JOURNAL_TRANSACTION");
    auto next=Advance(digests_,pending_);Reserve(records_);
    WriteEnvelope({{"type","commit_marker"},{"batch_id",pending_.at("batch_id")},{"prepared_sha256",prepared_checksum_},
        {"trace_revision",std::to_string(trace.trace_revision)},{"position",Pos(trace.tail_position)}},true);
    records_.push_back(std::move(pending_));pending_=nullptr;begin_=nullptr;digests_=std::move(next);
}
void DurableJournal::Close(){if(file_)file_->Flush();file_.reset();closed_=true;}
JournalRecovery DurableJournal::Recover(const std::filesystem::path& path,bool repair){
    platform::DurableFile file(path,platform::DurableFile::Mode::OpenExisting);auto text=file.ReadAll();
    JournalRecovery out;Json pending,begin;std::string previous(64,'0');uint64_t seq=0,revision=0;size_t at=0,safe=0,prepare_at=0;
    while(at<text.size()){
        auto end=text.find('\n',at);if(end==std::string::npos)break;
        Json envelope;
        try{envelope=Json::parse(text.substr(at,end-at));}
        catch(...){throw std::runtime_error("E_JOURNAL_CORRUPT");}
        auto saved=envelope.at("sha256").get<std::string>();envelope.erase("sha256");
        if(envelope.at("previous")!=previous || U64(envelope.at("seq"))!=seq+1 || saved!=Hash(envelope.dump()))
            throw std::runtime_error("E_JOURNAL_CHECKSUM");
        const auto& p=envelope.at("payload");auto type=p.at("type").get<std::string>();
        if(type=="record"){
            if(!pending.is_null() || !begin.is_null())throw std::runtime_error("E_JOURNAL_CORRUPT");out.records.push_back(p.at("record"));safe=end+1;
        }else if(type=="begin"){
            if(!pending.is_null() || !begin.is_null())throw std::runtime_error("E_JOURNAL_CORRUPT");begin=p;prepare_at=at;
        }else if(type=="prepare"){
            if(!pending.is_null())throw std::runtime_error("E_JOURNAL_CORRUPT");pending=p.at("transaction");
            if(begin.is_null())prepare_at=at;
            else if(begin.at("batch_id")!=pending.at("batch_id") || begin.at("evaluation_position")!=pending.at("evaluation_position") ||
                    begin.at("effective_inputs")!=pending.at("effective_inputs"))throw std::runtime_error("E_JOURNAL_CORRUPT");
        }else if(type=="commit_marker"){
            if(pending.is_null() || p.at("prepared_sha256")!=Hash(pending.dump()) || p.at("batch_id")!=pending.at("batch_id") ||
               p.at("position")!=pending.at("position") || p.at("trace_revision")!=pending.at("trace_revision") ||
               U64(p.at("trace_revision"))!=revision+1)throw std::runtime_error("E_JOURNAL_MARKER");
            ++revision;out.records.push_back(pending);pending=nullptr;begin=nullptr;safe=end+1;
        }else throw std::runtime_error("E_JOURNAL_CORRUPT");
        previous=saved;++seq;at=end+1;
    }
    if(!pending.is_null() || !begin.is_null())safe=prepare_at;
    out.valid_bytes=safe;out.truncated_bytes=text.size()-safe;
    if(out.records.empty() || out.records.at(0).at("kind")!="run_header")throw std::runtime_error("E_JOURNAL_HEADER");
    if(repair && out.truncated_bytes)file.Truncate(safe);
    return out;
}
JournalDigests DurableJournal::Digests(const Json& records){
    JournalDigests digest{Hash("sigflow.circuit.digest.v1"),Hash("sigflow.control.digest.v1")};
    for(const auto& record:records)digest=Advance(std::move(digest),record);return digest;
}
std::shared_ptr<LiveTraceStore> DurableJournal::RestoreTrace(const Json& records){
    const auto& header=records.at(0);std::vector<LiveSignal> signals;
    for(const auto& s:header.at("trace_signals"))signals.push_back({s.at("id"),s.at("name"),s.at("width"),s.at("kind")});
    auto store=std::make_shared<LiveTraceStore>(header.at("trace_run_id"),U64(header.at("trace_generation")),signals);
    store->BindWriterToCurrentThread();
    for(const auto& r:records)if(r.at("kind")=="commit"){
        std::vector<LiveTransition> events;for(const auto& t:r.at("trace_events"))events.push_back(TraceEventFromJson(t));
        store->AppendBatch(events);auto committed=store->Commit(Position(r.at("position")));
        if(std::to_string(committed.trace_revision)!=r.at("trace_revision").get<std::string>())throw std::runtime_error("E_JOURNAL_TRACE");
    }store->Seal();return store;
}
}
