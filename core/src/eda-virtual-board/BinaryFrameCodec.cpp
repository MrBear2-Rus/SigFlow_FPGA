#include "BinaryFrameCodec.h"
#include "eda/api/logic_state.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace eda::virtual_board {
namespace {
constexpr uint16_t RT_INFO=1,RT_DICTIONARY=2,RT_ACKS=3,RT_PAD_CHANGES=4,RT_COMPONENTS=5,RT_TRACE_CURSOR=6,RT_TRACE_EVENTS=7,RT_EVENTS=8,RT_DIAGNOSTICS=9;
void Put8(std::vector<uint8_t>& out,uint8_t v){out.push_back(v);}
void Put16(std::vector<uint8_t>& out,uint16_t v){out.push_back(uint8_t(v));out.push_back(uint8_t(v>>8));}
void Put32(std::vector<uint8_t>& out,uint32_t v){for(int i=0;i<4;++i)out.push_back(uint8_t(v>>(i*8)));}
void Put64(std::vector<uint8_t>& out,uint64_t v){for(int i=0;i<8;++i)out.push_back(uint8_t(v>>(i*8)));}
void Set32(std::vector<uint8_t>& out,size_t at,uint32_t v){for(int i=0;i<4;++i)out[at+size_t(i)]=uint8_t(v>>(i*8));}
void PutString(std::vector<uint8_t>& out,const std::string& s){
    if(s.size()>65535)throw std::runtime_error("E_SFVB_STRING_LENGTH");
    Put16(out,uint16_t(s.size()));out.insert(out.end(),s.begin(),s.end());
}
struct Reader {
    const uint8_t* data;size_t size,at=0;
    void Need(size_t n){if(size-at<n)throw std::runtime_error("E_SFVB_TRUNCATED");}
    uint8_t U8(){Need(1);return data[at++];}
    uint16_t U16(){Need(2);auto v=uint16_t(data[at]|(data[at+1]<<8));at+=2;return v;}
    uint32_t U32(){Need(4);uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(data[at+i])<<(i*8);at+=4;return v;}
    uint64_t U64(){Need(8);uint64_t v=0;for(int i=0;i<8;++i)v|=uint64_t(data[at+i])<<(i*8);at+=8;return v;}
    std::string Str(){auto n=U16();Need(n);std::string s(reinterpret_cast<const char*>(data+at),n);at+=n;return s;}
    std::string Blob(){auto n=U32();Need(n);std::string s(reinterpret_cast<const char*>(data+at),n);at+=n;return s;}
    bool Done()const{return at==size;}
};
uint8_t AckCode(const std::string& status){
    if(status=="applied")return 1;if(status=="pending")return 2;if(status=="superseded")return 3;
    if(status=="blocked")return 4;if(status=="rejected")return 5;if(status=="observed")return 6;if(status=="cancelled")return 7;
    throw std::runtime_error("E_SFVB_STATUS");
}
std::string AckStatus(uint8_t code){
    switch(code){case 1:return "applied";case 2:return "pending";case 3:return "superseded";case 4:return "blocked";
        case 5:return "rejected";case 6:return "observed";case 7:return "cancelled";default:throw std::runtime_error("E_SFVB_STATUS");}
}
uint64_t TickOf(const Json& j){auto s=j.get<std::string>();if(s.empty() || s.size()>20 || s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("E_SFVB_TICK_STRING");return std::stoull(s);}
std::string U64String(uint64_t v){return std::to_string(v);}
}
const char* SfvbKindName(uint16_t kind){
    switch(kind){case uint16_t(SfvbKind::Snapshot):return "snapshot";case uint16_t(SfvbKind::Delta):return "delta";
        case uint16_t(SfvbKind::Trace):return "trace";case uint16_t(SfvbKind::Display):return "display";default:throw std::runtime_error("E_SFVB_KIND");}
}
Json SfvbFrameToJson(const SfvbFrame& f){
    Json dictionary=Json::array();
    for(const auto& [id,kind,name]:f.dictionary)dictionary.push_back({{"name",name},{"kind",kind==0?"signal":kind==1?"node":"component"},{"numeric_id",U64String(id)}});
    Json acks=Json::array();for(const auto& a:f.acks)acks.push_back({{"applied_delta",a.applied_delta},{"applied_order",U64String(a.applied_order)},
        {"applied_tick",U64String(a.applied_tick)},{"command_id",a.command_id},{"status",a.status}});
    Json pads=Json::array();for(const auto& p:f.pad_changes)pads.push_back({{"contention",p.contention},{"logic",std::string(1,p.logic)},
        {"numeric_id",U64String(p.numeric_id)},{"state_code",p.state_code},{"target",p.target},{"valid",p.valid}});
    Json components=Json::array();for(const auto& c:f.components)components.push_back({{"id",c.component},{"numeric_id",U64String(c.numeric_id)},{"state",c.state}});
    Json events=Json::array();for(const auto& e:f.events)events.push_back({{"detail",e.detail},{"type",e.type}});
    Json trace=Json::array();for(const auto& t:f.trace_events)trace.push_back({{"contention",t.contention},{"delta",t.delta},{"event_seq",U64String(t.event_seq)},
        {"has_state_code",t.has_state_code},{"numeric_id",U64String(t.numeric_id)},{"state_code",t.has_state_code?Json(t.state_code):Json()},
        {"tick",U64String(t.tick)},{"valid",t.valid},{"value",std::string(1,t.value)}});
    Json out=Json::object();
    out["acks"]=acks;out["base_seq"]=U64String(f.base_seq);out["committed_delta"]=f.delta;out["committed_tick"]=U64String(f.tick);
    out["components"]=components;out["diagnostics"]=f.diagnostics;out["error"]=f.error;out["events"]=events;
    out["generation"]=U64String(f.generation);out["kind"]=SfvbKindName(f.kind);out["run_handle"]=U64String(f.run_handle);
    out["run_id"]=f.run_id;out["schema"]=f.schema;out["seq"]=U64String(f.seq);out["state"]=f.state;
    out["state_encoding"]=f.state_encoding;out["tick_ps"]=U64String(f.tick_ps);out["trace_events"]=trace;
    out["trace"]={{"committed_event_seq",U64String(f.trace.committed_event_seq)},{"committed_through_tick",U64String(f.trace.committed_through_tick)},
        {"cursor",f.trace.cursor},{"revision",U64String(f.trace.revision)}};
    out["pad_changes"]=pads;out["reason"]=f.reason;
    if(!dictionary.empty())out["dictionary"]=dictionary;
    return out;
}
SfvbFrame SfvbFrameFromJson(const Json& j){
    if(!j.is_object() || j.at("schema")!=std::string(SfvbFrameSchema))throw std::runtime_error("E_SFVB_JSON_SCHEMA");
    SfvbFrame f;std::string kind=j.at("kind").get<std::string>();
    for(auto [name,value]:std::initializer_list<std::pair<const char*,uint16_t>>{{"snapshot",uint16_t(SfvbKind::Snapshot)},{"delta",uint16_t(SfvbKind::Delta)},{"trace",uint16_t(SfvbKind::Trace)},{"display",uint16_t(SfvbKind::Display)}})
        if(kind==name)f.kind=value;
    if(!f.kind)throw std::runtime_error("E_SFVB_KIND");
    f.run_id=j.at("run_id").get<std::string>();f.run_handle=uint32_t(TickOf(j.at("run_handle")));f.generation=uint32_t(TickOf(j.at("generation")));
    f.seq=TickOf(j.at("seq"));f.base_seq=TickOf(j.at("base_seq"));f.tick_ps=TickOf(j.at("tick_ps"));
    f.tick=TickOf(j.at("committed_tick"));f.delta=j.at("committed_delta").get<uint32_t>();
    f.state=j.at("state").get<std::string>();f.reason=j.at("reason").get<std::string>();f.error=j.at("error").get<std::string>();
    f.state_encoding=j.at("state_encoding").get<std::string>();
    if(j.contains("dictionary"))for(const auto& d:j.at("dictionary")){
        auto name=d.at("kind").get<std::string>();
        f.dictionary.push_back({uint32_t(TickOf(d.at("numeric_id"))),name=="signal"?uint8_t(0):name=="node"?uint8_t(1):uint8_t(2),d.at("name").get<std::string>()});}
    for(const auto& a:j.at("acks"))f.acks.push_back({a.at("command_id").get<std::string>(),a.at("status").get<std::string>(),TickOf(a.at("applied_tick")),a.at("applied_delta").get<uint32_t>(),TickOf(a.at("applied_order"))});
    for(const auto& p:j.at("pad_changes")){auto logic=p.at("logic").get<std::string>();if(logic.size()!=1)throw std::runtime_error("E_SFVB_JSON_LOGIC");
        f.pad_changes.push_back({uint32_t(TickOf(p.at("numeric_id"))),p.at("target").get<std::string>(),p.at("state_code").get<uint8_t>(),logic[0],p.at("valid").get<bool>(),p.at("contention").get<bool>()});}
    for(const auto& c:j.at("components"))f.components.push_back({uint32_t(TickOf(c.at("numeric_id"))),c.at("id").get<std::string>(),c.at("state")});
    for(const auto& e:j.at("events"))f.events.push_back({e.at("type").get<std::string>(),e.at("detail")});
    for(const auto& t:j.at("trace_events")){auto value=t.at("value").get<std::string>();if(value.size()!=1)throw std::runtime_error("E_SFVB_JSON_VALUE");
        f.trace_events.push_back({uint32_t(TickOf(t.at("numeric_id"))),TickOf(t.at("tick")),TickOf(t.at("event_seq")),t.at("delta").get<uint32_t>(),value[0],
            t.at("state_code").is_null()?uint8_t(0):t.at("state_code").get<uint8_t>(),t.at("valid").get<bool>(),t.at("contention").get<bool>(),t.at("has_state_code").get<bool>()});}
    const auto& trace=j.at("trace");f.trace={TickOf(trace.at("revision")),TickOf(trace.at("committed_through_tick")),TickOf(trace.at("committed_event_seq")),trace.at("cursor").get<std::string>()};
    f.diagnostics=j.at("diagnostics");
    return f;
}
std::vector<uint8_t> SfvbEncodeFrame(const SfvbFrame& f){
    if(f.kind==0 || f.run_id.size()>65535)throw std::runtime_error("E_SFVB_KIND");
    if(f.kind==uint16_t(SfvbKind::Snapshot)){if(f.base_seq!=0)throw std::runtime_error("E_SFVB_SNAPSHOT_BASE");
        if(f.dictionary.empty())throw std::runtime_error("E_SFVB_DICTIONARY");}
    else if(!f.dictionary.empty())throw std::runtime_error("E_SFVB_DELTA_DICTIONARY");
    std::set<uint32_t> ids;for(const auto& [id,kind,name]:f.dictionary){if(!ids.insert(id).second || name.empty())throw std::runtime_error("E_SFVB_DICTIONARY");}
    std::vector<uint8_t> payload;size_t records=0;
    Put16(payload,RT_INFO);auto info=payload.size();Put32(payload,0);
    PutString(payload,f.schema);PutString(payload,f.run_id);PutString(payload,f.state_encoding);
    PutString(payload,f.state);PutString(payload,f.reason);PutString(payload,f.error);Put64(payload,f.tick_ps);
    Set32(payload,info,uint32_t(payload.size()-info-4));++records;
    if(!f.dictionary.empty()){
        Put16(payload,RT_DICTIONARY);auto body=payload.size();Put32(payload,0);
        Put32(payload,uint32_t(f.dictionary.size()));for(const auto& [id,kind,name]:f.dictionary){Put32(payload,id);Put8(payload,kind);PutString(payload,name);}
        Set32(payload,body,uint32_t(payload.size()-body-4));++records;
    }
    Put16(payload,RT_ACKS);auto acks=payload.size();Put32(payload,0);
    Put32(payload,uint32_t(f.acks.size()));for(const auto& a:f.acks){PutString(payload,a.command_id);Put8(payload,AckCode(a.status));
        Put64(payload,a.applied_tick);Put32(payload,a.applied_delta);Put64(payload,a.applied_order);}
    Set32(payload,acks,uint32_t(payload.size()-acks-4));++records;
    Put16(payload,RT_PAD_CHANGES);auto pads=payload.size();Put32(payload,0);
    Put32(payload,uint32_t(f.pad_changes.size()));for(const auto& p:f.pad_changes){
        Put32(payload,p.numeric_id);Put8(payload,p.state_code);Put8(payload,uint8_t(p.logic));
        Put8(payload,uint8_t((p.valid?1:0)|(p.contention?2:0)));}
    Set32(payload,pads,uint32_t(payload.size()-pads-4));++records;
    Put16(payload,RT_COMPONENTS);auto comps=payload.size();Put32(payload,0);
    Put32(payload,uint32_t(f.components.size()));for(const auto& c:f.components){
        auto json=c.state.dump();Put32(payload,c.numeric_id);Put32(payload,uint32_t(json.size()));payload.insert(payload.end(),json.begin(),json.end());}
    Set32(payload,comps,uint32_t(payload.size()-comps-4));++records;
    Put16(payload,RT_TRACE_CURSOR);auto cursor=payload.size();Put32(payload,0);
    Put64(payload,f.trace.revision);Put64(payload,f.trace.committed_through_tick);Put64(payload,f.trace.committed_event_seq);PutString(payload,f.trace.cursor);
    Set32(payload,cursor,uint32_t(payload.size()-cursor-4));++records;
    Put16(payload,RT_TRACE_EVENTS);auto tr=payload.size();Put32(payload,0);
    Put32(payload,uint32_t(f.trace_events.size()));for(const auto& t:f.trace_events){
        Put32(payload,t.numeric_id);Put64(payload,t.tick);Put32(payload,t.delta);Put64(payload,t.event_seq);
        Put8(payload,uint8_t(t.value));Put8(payload,t.state_code);Put8(payload,uint8_t((t.valid?1:0)|(t.contention?2:0)|(t.has_state_code?4:0)));}
    Set32(payload,tr,uint32_t(payload.size()-tr-4));++records;
    Put16(payload,RT_EVENTS);auto ev=payload.size();Put32(payload,0);
    Put32(payload,uint32_t(f.events.size()));for(const auto& e:f.events){PutString(payload,e.type);auto json=e.detail.dump();
        Put32(payload,uint32_t(json.size()));payload.insert(payload.end(),json.begin(),json.end());}
    Set32(payload,ev,uint32_t(payload.size()-ev-4));++records;
    Put16(payload,RT_DIAGNOSTICS);auto diag=payload.size();Put32(payload,0);
    auto json=f.diagnostics.dump();Put32(payload,uint32_t(json.size()));payload.insert(payload.end(),json.begin(),json.end());
    Set32(payload,diag,uint32_t(payload.size()-diag-4));++records;
    if(payload.size()>SfvbMaxPayloadBytes)throw std::runtime_error("E_SFVB_PAYLOAD_QUOTA");
    std::vector<uint8_t> out;out.reserve(SfvbHeaderBytes+payload.size());
    out.push_back('S');out.push_back('F');out.push_back('V');out.push_back('B');
    Put16(out,SfvbVersion);Put16(out,f.kind);Put32(out,f.flags);Put32(out,SfvbHeaderBytes);
    Put32(out,uint32_t(payload.size()));Put32(out,f.run_handle);Put32(out,f.generation);
    Put64(out,f.seq);Put64(out,f.base_seq);Put64(out,f.tick);
    Put32(out,f.delta);Put32(out,uint32_t(records));Put32(out,0);
    out.insert(out.end(),payload.begin(),payload.end());return out;
}
SfvbFrame SfvbDecodeFrame(const uint8_t* data,size_t size){
    Reader r{data,size};
    if(size<SfvbHeaderBytes)throw std::runtime_error("E_SFVB_HEADER");
    if(data[0]!='S'||data[1]!='F'||data[2]!='V'||data[3]!='B')throw std::runtime_error("E_SFVB_MAGIC");
    r.at=4;auto version=r.U16(),kind=r.U16();auto flags=r.U32(),header=r.U32();
    auto payloadBytes=r.U32();auto runHandle=r.U32(),generation=r.U32();
    auto seq=r.U64(),baseSeq=r.U64(),tick=r.U64();
    auto delta=r.U32(),recordCount=r.U32(),reserved=r.U32();
    if(version!=SfvbVersion)throw std::runtime_error("E_SFVB_VERSION");
    if(header!=SfvbHeaderBytes || reserved!=0)throw std::runtime_error("E_SFVB_HEADER");
    if(payloadBytes!=size-SfvbHeaderBytes || payloadBytes>SfvbMaxPayloadBytes)throw std::runtime_error("E_SFVB_PAYLOAD");
    SfvbFrame f;f.kind=kind;f.flags=flags;f.run_handle=runHandle;f.generation=generation;
    f.seq=seq;f.base_seq=baseSeq;f.tick=tick;f.delta=delta;
    r.at=SfvbHeaderBytes;size_t records=0;
    auto next=[&]{if(r.Done())return uint16_t(0);return r.U16();};
    if(next()!=RT_INFO)throw std::runtime_error("E_SFVB_RECORD_ORDER");
    {auto len=r.U32();auto end=r.at+len;f.schema=r.Str();f.run_id=r.Str();f.state_encoding=r.Str();
        f.state=r.Str();f.reason=r.Str();f.error=r.Str();f.tick_ps=r.U64();if(r.at!=end)throw std::runtime_error("E_SFVB_RECORD_SIZE");}
    ++records;
    if(f.schema!=std::string(SfvbFrameSchema))throw std::runtime_error("E_SFVB_SCHEMA");
    auto type=next();
    if(type==RT_DICTIONARY){
        if(f.kind!=uint16_t(SfvbKind::Snapshot))throw std::runtime_error("E_SFVB_DELTA_DICTIONARY");
        auto len=r.U32();auto end=r.at+len;auto count=r.U32();
        for(uint32_t i=0;i<count;++i){auto id=r.U32();auto kindByte=r.U8();f.dictionary.push_back({id,kindByte,r.Str()});}
        if(r.at!=end)throw std::runtime_error("E_SFVB_RECORD_SIZE");++records;type=next();
    }
    if(type==RT_ACKS){auto len=r.U32();auto end=r.at+len;auto count=r.U32();
        for(uint32_t i=0;i<count;++i){auto id=r.Str();auto code=r.U8();f.acks.push_back({id,AckStatus(code),r.U64(),r.U32(),r.U64()});}
        if(r.at!=end)throw std::runtime_error("E_SFVB_RECORD_SIZE");++records;type=next();}
    if(type==RT_PAD_CHANGES){auto len=r.U32();auto end=r.at+len;auto count=r.U32();
        for(uint32_t i=0;i<count;++i){auto id=r.U32();auto stateCode=r.U8();auto logic=r.U8();auto flag=r.U8();
            f.pad_changes.push_back({id,std::string(),stateCode,char(logic),bool(flag&1),bool(flag&2)});}
        if(r.at!=end)throw std::runtime_error("E_SFVB_RECORD_SIZE");++records;type=next();}
    if(type==RT_COMPONENTS){auto len=r.U32();auto end=r.at+len;auto count=r.U32();
        for(uint32_t i=0;i<count;++i){auto id=r.U32();auto blob=r.Blob();f.components.push_back({id,std::string(),Json::parse(blob)});}
        if(r.at!=end)throw std::runtime_error("E_SFVB_RECORD_SIZE");++records;type=next();}
    if(type==RT_TRACE_CURSOR){auto len=r.U32();auto end=r.at+len;f.trace.revision=r.U64();f.trace.committed_through_tick=r.U64();
        f.trace.committed_event_seq=r.U64();f.trace.cursor=r.Str();
        if(r.at!=end)throw std::runtime_error("E_SFVB_RECORD_SIZE");++records;type=next();}
    if(type==RT_TRACE_EVENTS){auto len=r.U32();auto end=r.at+len;auto count=r.U32();
        for(uint32_t i=0;i<count;++i){auto id=r.U32();auto tickv=r.U64();auto dv=r.U32();auto ev=r.U64();
            auto value=r.U8();auto stateCode=r.U8();auto flag=r.U8();
            f.trace_events.push_back({id,tickv,ev,dv,char(value),stateCode,bool(flag&1),bool(flag&2),bool(flag&4)});}
        if(r.at!=end)throw std::runtime_error("E_SFVB_RECORD_SIZE");++records;type=next();}
    if(type==RT_EVENTS){auto len=r.U32();auto end=r.at+len;auto count=r.U32();
        for(uint32_t i=0;i<count;++i){auto name=r.Str();auto blob=r.Blob();f.events.push_back({name,Json::parse(blob)});}
        if(r.at!=end)throw std::runtime_error("E_SFVB_RECORD_SIZE");++records;type=next();}
    if(type==RT_DIAGNOSTICS){auto len=r.U32();auto end=r.at+len;f.diagnostics=Json::parse(r.Blob());
        if(r.at!=end)throw std::runtime_error("E_SFVB_RECORD_SIZE");++records;type=next();}
    if(type!=0 || !r.Done())throw std::runtime_error("E_SFVB_RECORD_ORDER");
    if(records!=recordCount)throw std::runtime_error("E_SFVB_RECORD_COUNT");
    if(f.kind==uint16_t(SfvbKind::Snapshot) && f.base_seq!=0)throw std::runtime_error("E_SFVB_SNAPSHOT_BASE");
    // A full snapshot is self-describing: resolve numeric ids against its own
    // dictionary. Deltas are resolved by the stateful stream decoder.
    if(!f.dictionary.empty()){
        std::map<uint32_t,std::string> names;for(const auto& [id,kind,name]:f.dictionary)names[id]=name;
        for(auto& pad:f.pad_changes){auto it=names.find(pad.numeric_id);if(it==names.end())throw std::runtime_error("E_SFVB_NUMERIC_ID");pad.target=it->second;}
        for(auto& comp:f.components){auto it=names.find(comp.numeric_id);if(it==names.end())throw std::runtime_error("E_SFVB_NUMERIC_ID");comp.component=it->second;}
        for(const auto& trace:f.trace_events)if(!names.count(trace.numeric_id))throw std::runtime_error("E_SFVB_NUMERIC_ID");
    }
    return f;
}
SfvbStreamDecoder::Result SfvbStreamDecoder::Feed(const uint8_t* data,size_t size){
    Result result;
    try{
        auto frame=SfvbDecodeFrame(data,size);
        if(!has_baseline_){
            if(frame.kind!=uint16_t(SfvbKind::Snapshot))return {Status::NeedFullSnapshot,frame,"E_SFVB_BASELINE_MISSING"};
            if(frame.base_seq!=0 || frame.dictionary.empty())throw std::runtime_error("E_SFVB_SNAPSHOT_BASE");
            run_handle_=frame.run_handle;generation_=frame.generation;dictionary_.clear();
            for(const auto& [id,kind,name]:frame.dictionary)dictionary_[id]=name;
            has_baseline_=true;last_seq_=frame.seq;
            for(auto& pad:frame.pad_changes)if(!dictionary_.count(pad.numeric_id))throw std::runtime_error("E_SFVB_NUMERIC_ID");else pad.target=dictionary_.at(pad.numeric_id);
            for(auto& comp:frame.components)if(!dictionary_.count(comp.numeric_id))throw std::runtime_error("E_SFVB_NUMERIC_ID");else comp.component=dictionary_.at(comp.numeric_id);
            result.frame=std::move(frame);return result;
        }
        if(frame.run_handle!=run_handle_ || frame.generation!=generation_)return {Status::StaleGeneration,frame,"E_SFVB_GENERATION_STALE"};
        if(frame.seq!=last_seq_+1)return {Status::NeedFullSnapshot,frame,"E_SFVB_SEQ_GAP"};
        if(frame.kind==uint16_t(SfvbKind::Snapshot)){
            if(frame.base_seq!=0 || frame.dictionary.empty())throw std::runtime_error("E_SFVB_SNAPSHOT_BASE");
            dictionary_.clear();for(const auto& [id,kind,name]:frame.dictionary)dictionary_[id]=name;
        }else if(frame.base_seq!=last_seq_)return {Status::NeedFullSnapshot,frame,"E_SFVB_BASELINE_LOST"};
        for(auto& pad:frame.pad_changes)if(!dictionary_.count(pad.numeric_id))throw std::runtime_error("E_SFVB_NUMERIC_ID");else pad.target=dictionary_.at(pad.numeric_id);
        for(auto& comp:frame.components)if(!dictionary_.count(comp.numeric_id))throw std::runtime_error("E_SFVB_NUMERIC_ID");else comp.component=dictionary_.at(comp.numeric_id);
        for(const auto& trace:frame.trace_events)if(!dictionary_.count(trace.numeric_id))throw std::runtime_error("E_SFVB_NUMERIC_ID");
        last_seq_=frame.seq;
        result.frame=std::move(frame);return result;
    }catch(const std::exception& e){return {Status::FormatError,SfvbFrame{},e.what()};}
}
SfvbFrameBuilder::SfvbFrameBuilder(uint32_t run_handle,uint32_t generation,const std::vector<LiveSignal>& signals):run_handle_(run_handle),generation_(generation){
    uint32_t top=0;for(const auto& s:signals){if(ids_.count(s.name) || !signal_ids_.insert(s.id).second)throw std::runtime_error("E_SFVB_DUPLICATE_SIGNAL");
        ids_[s.name]=s.id;top=std::max(top,s.id+1);}
    next_id_=top;
}
SfvbFrame SfvbFrameBuilder::Feed(const Json& snapshot,const std::vector<LiveTransition>& traceEvents){
    auto tick=TickOf(snapshot.at("position").at("tick"));auto delta=snapshot.at("position").at("delta").get<uint32_t>();
    auto order=TickOf(snapshot.at("position").at("event_seq"));
    auto runId=snapshot.at("run_id").get<std::string>();
    auto state=snapshot.at("state").get<std::string>();
    auto reason=snapshot.value("reason",std::string());auto error=snapshot.value("error",std::string());
    auto encoding=snapshot.contains("resolver")?snapshot.at("resolver").get<std::string>():std::string(ResolverVersion);
    // Node and component observations. Blind projections keep the same board
    // shape with sanitized explanations, so binary parity also covers
    // desensitized student runs.
    std::map<std::string,std::tuple<std::string,int,bool,bool>> nodes;
    if(snapshot.contains("board") && snapshot.at("board").contains("nodes"))
        for(const auto& [name,node]:snapshot.at("board").at("nodes").items())
            nodes[name]={node.at("logic").get<std::string>(),node.at("state_code").get<int>(),node.at("valid").get<bool>(),node.at("contention").get<bool>()};
    std::map<std::string,Json> components;
    if(snapshot.contains("board")){
        const auto& board=snapshot.at("board");
        if(board.contains("peripherals"))for(const auto& [id,value]:board.at("peripherals").items())components[id]=value;
        if(board.contains("leds"))for(const auto& [id,value]:board.at("leds").items())components[id]=value;
        if(board.contains("uart"))for(const auto& [id,value]:board.at("uart").items())components[id]=value;
    }
    Json faultEvents=Json::array();
    if(snapshot.contains("faults") && snapshot.at("faults").contains("events"))
        faultEvents=snapshot.at("faults").at("events");
    // Fault ledgers reset at every commit, so each frame carries the events of
    // the commit that produced it; no cross-frame event counting is possible.
    bool rebaseline=!seq_ || runId!=run_id_ || tick<previous_tick_;
    for(const auto& [name,_]:nodes)if(!ids_.count(name))rebaseline=true;
    for(const auto& [name,_]:components)if(!ids_.count(name))rebaseline=true;
    SfvbFrame frame;
    frame.kind=uint16_t(rebaseline?SfvbKind::Snapshot:SfvbKind::Delta);
    frame.run_handle=run_handle_;frame.generation=generation_;frame.seq=++seq_;
    frame.base_seq=rebaseline?0:seq_-1;frame.tick=tick;frame.delta=delta;
    frame.run_id=runId;frame.state=state;frame.reason=reason;frame.error=error;frame.state_encoding=encoding;
    if(rebaseline){
        std::vector<std::string> nodeNames,componentNames;
        for(const auto& [name,_]:nodes)nodeNames.push_back(name);
        for(const auto& [name,_]:components)componentNames.push_back(name);
        std::sort(nodeNames.begin(),nodeNames.end());std::sort(componentNames.begin(),componentNames.end());
        for(const auto& name:nodeNames)if(ids_.emplace(name,next_id_).second)++next_id_;
        for(const auto& name:componentNames)if(ids_.emplace(name,next_id_).second)++next_id_;
        // Signals keep their native LiveTrace ids; observation names follow.
        for(const auto& [name,id]:ids_)frame.dictionary.push_back({id,components.count(name)?uint8_t(2):nodes.count(name)?uint8_t(1):uint8_t(0),name});
        for(const auto& [name,node]:nodes){auto& [logic,code,valid,contention]=node;
            frame.pad_changes.push_back({ids_.at(name),name,uint8_t(code),logic.empty()?char('x'):logic[0],valid,contention});}
        for(const auto& [name,state]:components)frame.components.push_back({ids_.at(name),name,state});
    }else{
        for(const auto& [name,node]:nodes){auto it=previous_nodes_.find(name);
            if(it!=previous_nodes_.end() && it->second==node)continue;auto& [logic,code,valid,contention]=node;
            frame.pad_changes.push_back({ids_.at(name),name,uint8_t(code),logic.empty()?char('x'):logic[0],valid,contention});}
        for(const auto& [name,state]:components){auto it=previous_components_.find(name);
            if(it!=previous_components_.end() && it->second==state)continue;frame.components.push_back({ids_.at(name),name,state});}
    }
    if(snapshot.contains("trace")){const auto& trace=snapshot.at("trace");
        frame.trace.revision=TickOf(trace.at("revision"));frame.trace.committed_through_tick=TickOf(trace.at("committed_through_tick"));
        frame.trace.committed_event_seq=TickOf(trace.at("tail_event_seq"));
        frame.trace.cursor="chunk-"+std::to_string(frame.trace.revision)+":"+std::to_string(frame.trace.committed_event_seq);}
    if(snapshot.contains("command_id") && snapshot.contains("status"))
        frame.acks.push_back({snapshot.at("command_id").get<std::string>(),snapshot.at("status").get<std::string>(),tick,delta,order});
    for(const auto& event:faultEvents)frame.events.push_back({event.at("type").get<std::string>(),event});
    for(const auto& t:traceEvents){if(t.value.empty() || !signal_ids_.count(t.signal_id))throw std::runtime_error("E_SFVB_SIGNAL_ID");
        frame.trace_events.push_back({t.signal_id,t.position.tick,t.position.event_seq,t.position.delta,t.value[0],
            t.state_code?*t.state_code:uint8_t(0),t.valid,t.contention,bool(t.state_code)});}
    previous_nodes_=std::move(nodes);previous_components_=std::move(components);
    previous_tick_=tick;run_id_=runId;
    return frame;
}
}
