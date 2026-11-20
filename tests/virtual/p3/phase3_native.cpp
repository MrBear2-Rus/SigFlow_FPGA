#include "p3_fixtures.hpp"
#include "../k4_k5/TestCounter.h"
#include "eda-virtual-board/BinaryFrameCodec.h"
#include "eda-virtual-board/DurableJournal.h"
#include "eda-virtual-board/Phase1Peripherals.h"
#include "eda-virtual-board/Phase2Faults.h"
#include "eda-virtual-board/Phase3Faults.h"
#include "eda-virtual-board/State13.h"
#include "eda-virtual-board/BlindProjection.h"
#include "eda-platform/Platform.h"
#include <fstream>
#include <iostream>
namespace vb=eda::virtual_board;using eda::Json;namespace fs=std::filesystem;
size_t checks=0;void Check(bool yes,const std::string& s){++checks;if(!yes)throw std::runtime_error(s);}
Json Read(const fs::path& path){std::ifstream in(path);if(!in)throw std::runtime_error("E_FILE_READ");return Json::parse(in);}
void Write(const fs::path& path,const Json& value){if(!path.parent_path().empty())fs::create_directories(path.parent_path());std::ofstream out(path);out<<value.dump(2)<<"\n";}
template<typename F> std::string Error(F&& body){try{body();}catch(const std::exception& e){return e.what();}return {};}
std::vector<vb::LiveSignal> LoadSignals(const Json& reply){
    std::vector<vb::LiveSignal> signals;for(const auto& s:reply.at("signals"))signals.push_back({s.at("id"),s.at("name"),s.at("width"),s.at("kind")});return signals;
}
void UseReviewedElectrical(const fs::path& root,vb::SessionContract& c){
    auto file=root/"registry/electrical/tang-nano-9k.6202.reviewed.v2.json";
    std::vector<vb::Diagnostic> ds;if(!vb::LoadElectricalProfile(file,c.electrical,ds))throw std::runtime_error("profile");
    c.electrical_sha256=eda::platform::Sha256FileHex(file);
}
std::vector<uint8_t> LoadBytes(const fs::path& path){std::ifstream in(path,std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});return bytes;}
void SaveBytes(const fs::path& path,const std::vector<uint8_t>& bytes){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));}
uint64_t PayloadAt(const std::vector<uint8_t>& bytes,size_t at){uint64_t payload=0;for(int i=0;i<4;++i)payload|=uint64_t(bytes[at+16+size_t(i)])<<(i*8);return payload;}
// -------- native mode: codec algebra, live sessions, new faults, keypad ----
int RunNative(const fs::path& root,const fs::path& out){
    Json report;fs::create_directories(out);
    // 1. Frozen SFVB header layout: explicit little-endian, 64 bytes.
    {
        vb::SfvbFrame f;f.kind=uint16_t(vb::SfvbKind::Snapshot);f.run_handle=42;f.generation=1;f.seq=7;f.base_seq=0;f.tick=100000000000ULL;f.delta=3;
        f.run_id="run-demo-001";f.state="paused";f.reason="StepComplete";f.state_encoding=vb::State13Version;
        f.dictionary={{0,uint8_t(0),"port:clock_in"},{5,uint8_t(1),"pad.3"},{9,uint8_t(2),"uart0"}};
        f.acks={{"cmd-0007","applied",100000000000ULL,3,91}};
        f.pad_changes={{5,"pad.3",13,'0',true,false}};
        f.components={{9,"uart0",Json{{"received",Json::array()}}}};
        f.trace={12,100000000000ULL,800,"chunk-12:800"};
        f.trace_events={{5,100000000000ULL,800,3,'1',13,true,false,true}};
        f.events={{"pin.edge",Json{{"tick","100"}}}};
        auto bytes=vb::SfvbEncodeFrame(f);
        Check(bytes.size()>vb::SfvbHeaderBytes && bytes[0]=='S' && bytes[1]=='F' && bytes[2]=='V' && bytes[3]=='B',"magic bytes");
        Check(bytes[4]==1 && bytes[5]==0,"version 1 little endian");
        Check(bytes[6]==uint8_t(vb::SfvbKind::Snapshot) && bytes[7]==0,"frame type little endian");
        auto le32=[&](size_t at){return uint32_t(bytes[at])|uint32_t(bytes[at+1]<<8)|uint32_t(bytes[at+2]<<16)|uint32_t(bytes[at+3]<<24);};
        auto le64=[&](size_t at){uint64_t v=0;for(int i=0;i<8;++i)v|=uint64_t(bytes[at+size_t(i)])<<(i*8);return v;};
        Check(le32(8)==0 && le32(12)==64,"flags zero and header_bytes=64");
        Check(le32(16)==bytes.size()-64 && le32(20)==42 && le32(24)==1,"payload bytes, run handle and generation");
        Check(le64(28)==7 && le64(36)==0 && le64(44)==100000000000ULL,"seq/base/tick little endian");
        Check(le32(52)==3 && le32(60)==0,"delta and reserved");
        Check(Error([&]{vb::SfvbDecodeFrame(bytes.data(),bytes.size()-8);})=="E_SFVB_PAYLOAD","truncated payload rejected");
        auto damaged=bytes;damaged[0]='X';Check(Error([&]{vb::SfvbDecodeFrame(damaged.data(),damaged.size());})=="E_SFVB_MAGIC","magic gate");
        damaged=bytes;damaged[36]=1;
        Check(Error([&]{vb::SfvbDecodeFrame(damaged.data(),damaged.size());})=="E_SFVB_SNAPSHOT_BASE","snapshot base_seq gate");
        auto source=vb::SfvbFrameToJson(f);
        Check(vb::SfvbFrameToJson(vb::SfvbFrameFromJson(source))==source,"JSON frame inverse is lossless");
        auto decoded=vb::SfvbDecodeFrame(bytes.data(),bytes.size());
        Check(vb::SfvbFrameToJson(decoded)==source,"binary decode equals the JSON delta frame field by field");
        auto delta=f;delta.kind=uint16_t(vb::SfvbKind::Delta);delta.base_seq=6;
        Check(Error([&]{vb::SfvbEncodeFrame(delta);})=="E_SFVB_DELTA_DICTIONARY","delta frames cannot carry a dictionary");
        delta.dictionary.clear();
        auto bad=delta;bad.acks={{"cmd","invented",0,0,0}};
        Check(Error([&]{vb::SfvbEncodeFrame(bad);})=="E_SFVB_STATUS","unknown ack status rejected");
    }
    // 2. Stream continuity: baseline loss, sequence gaps, stale generations.
    {
        vb::SfvbFrameBuilder builder(42,1,{{0,"port:clock_in",1,"port"},{1,"port:led_n[0]",1,"port"}});
        Json base={{"schema","sigflow.virtual-frame.v1"},{"run_id","run-42"},{"state","Paused"},{"reason","Ready"},{"error",""},
            {"position",{{"tick","0"},{"delta",0},{"event_seq","0"}}},{"trace",{{"revision","0"},{"committed_through_tick","0"},{"tail_event_seq","0"}}},
            {"board",{{"nodes",{{"pad.3",{{"logic","1"},{"state_code",7},{"valid",true},{"contention",false}}}}},{"leds",Json::object()},{"peripherals",Json::object()},{"uart",Json::object()}}}};
        auto s0=builder.Feed(base);
        Json next=base;next["position"]={{"tick","18519"},{"delta",0},{"event_seq","1"}};next["board"]["nodes"]["pad.3"]={{"logic","0"},{"state_code",13},{"valid",true},{"contention",false}};
        next["command_id"]="step-1";next["status"]="applied";next["reason"]="StepComplete";
        auto s1=builder.Feed(next);
        Json more=next;more["position"]={{"tick","37037"},{"delta",0},{"event_seq","2"}};more["command_id"]="step-2";
        auto s2=builder.Feed(more);
        Check(s0.kind==uint16_t(vb::SfvbKind::Snapshot) && s1.kind==uint16_t(vb::SfvbKind::Delta) && s1.base_seq==s0.seq && s2.base_seq==s1.seq,"snapshot then chained deltas");
        auto b0=vb::SfvbEncodeFrame(s0),b1=vb::SfvbEncodeFrame(s1),b2=vb::SfvbEncodeFrame(s2);
        vb::SfvbStreamDecoder stream;
        auto r0=stream.Feed(b0.data(),b0.size());Check(r0.status==vb::SfvbStreamDecoder::Status::Ok && stream.LastSeq()==s0.seq,"baseline established");
        Check(r0.frame.pad_changes.at(0).target=="pad.3","numeric ids resolve through the dictionary");
        Check(stream.Feed(b1.data(),b1.size()).status==vb::SfvbStreamDecoder::Status::Ok && stream.Feed(b2.data(),b2.size()).status==vb::SfvbStreamDecoder::Status::Ok,"continuous deltas accepted");
        vb::SfvbStreamDecoder gapped;gapped.Feed(b0.data(),b0.size());
        auto gap=gapped.Feed(b2.data(),b2.size());Check(gap.status==vb::SfvbStreamDecoder::Status::NeedFullSnapshot && gap.error=="E_SFVB_SEQ_GAP","sequence gap demands a full snapshot");
        auto shifted=s1;shifted.base_seq+=1;auto sb=vb::SfvbEncodeFrame(shifted);
        vb::SfvbStreamDecoder tampered;tampered.Feed(b0.data(),b0.size());
        auto lost=tampered.Feed(sb.data(),sb.size());Check(lost.status==vb::SfvbStreamDecoder::Status::NeedFullSnapshot && lost.error=="E_SFVB_BASELINE_LOST","mismatched base_seq refuses the delta");
        auto stale=s1;stale.generation+=1;auto st=vb::SfvbEncodeFrame(stale);
        vb::SfvbStreamDecoder aged;aged.Feed(b0.data(),b0.size());
        auto dropped=aged.Feed(st.data(),st.size());Check(dropped.status==vb::SfvbStreamDecoder::Status::StaleGeneration && dropped.error=="E_SFVB_GENERATION_STALE","stale generation discarded without consuming seq");
        Check(aged.Feed(b1.data(),b1.size()).status==vb::SfvbStreamDecoder::Status::Ok,"stream continues after discarding the stale callback");
        vb::SfvbStreamDecoder noBase;
        Check(noBase.Feed(b1.data(),b1.size()).error=="E_SFVB_BASELINE_MISSING","delta before any snapshot requests a full one");
        auto rebased=s0;rebased.seq=s2.seq+1;rebased.tick=55556;auto rb=vb::SfvbEncodeFrame(rebased);
        Check(stream.Feed(rb.data(),rb.size()).status==vb::SfvbStreamDecoder::Status::Ok && stream.LastSeq()==rebased.seq,"mid-stream snapshot re-baselines the dictionary");
    }
    Write(out/"progress.json",{{"section",3}});
    // 3. Live P2 session trajectory: four P1 faults + bridge contention +
    //    PWM segments + bit-level UART, student-projected before encoding.
    {
        auto c=m0test::Contract(root);auto descriptor=c.board->Descriptor();
        descriptor["electrical"]["resolver_version"]=vb::State13Version;descriptor["electrical"]["state_encoding"]=vb::State13Version;
        descriptor["version"]="2.0.0";c.board=std::make_shared<const vb::BoardTopology>(descriptor,c.electrical);
        c.phase2=true;c.fault_profile=vb::Phase1Faults::EmptyProfile(*c.board);
        auto rule=[](std::string id,std::string type,std::string target,std::string kind,std::string hook,Json effect=Json::object(),Json trigger=Json{{"kind","permanent"},{"start_tick","0"}})->Json{
            effect["type"]=type;return {{"id",id},{"enabled",true},{"visibility","teacher_only"},{"target",{{"id",target},{"kind",kind}}},{"hook",hook},{"effect",effect},{"trigger",trigger}};};
        Json edges=Json::array();edges.push_back({{"dt","0"},{"contact","closed"}});
        for(auto dt:{"2","5","9"})edges.push_back({{"dt",dt},{"contact","released"}});
        edges.push_back({{"dt","14"},{"contact","closed"}});
        c.fault_profile["faults"]={
            rule("hidden-stuck","stuck_at","fpga0.pad.10","fpga_pad_driver","driver.transform",{{"mode","replace_source"},{"value",1}}),
            rule("hidden-open","open","wire.led1","connection","topology.transform"),
            rule("hidden-bounce","button_bounce","key_p3","component","peripheral.event",{{"drive","open_drain"},{"duration_ticks","14"},{"relative_edges",edges}},
                {{"kind","intermittent"},{"event",{{"type","input.intent"},{"target","key_p3"},{"action","press"},{"occurrence",1}}},{"probability_ppm",1000000},{"active_ticks","200"},{"cooldown_ticks","500"},{"max_activations",1}}),
            rule("hidden-clock","clock_stop","clk27m","clock","clock.schedule",{{"hold_level",0},{"resume_phase","restart_half_period"}},
                {{"kind","transient"},{"start_tick","3000000"},{"end_tick","6000000"}}),
            rule("hidden-bridge","bridge","pad.10","node","topology.transform",{{"other_node","pad.11"}})};
        vb::VirtualSession s(c,m0test::Factory());
        std::vector<Json> trajectory;
        trajectory.push_back(s.Submit({{"op","get"}}));
        trajectory.push_back(s.Submit(m0test::Set("reset_p4",2,"release")));
        trajectory.push_back(s.Submit(m0test::Set("key_p3",13,"press")));
        trajectory.push_back(s.Submit(m0test::Cmd("step","s1")));
        trajectory.push_back(s.Submit({{"op","uart.send"},{"command_id","u1"},{"target","uart0"},{"bytes",Json::array({65,66})}}));
        trajectory.push_back(s.Submit({{"op","dip.set"},{"command_id","d1"},{"target","dip0"},{"value",2}}));
        trajectory.push_back(s.Submit({{"op","step"},{"command_id","s2"},{"mode","ticks"},{"ticks","4000000"}}));
        Check(trajectory.back().at("status")=="applied" && trajectory.back().at("reason")=="StepComplete","faulted session advances through UART/DIP/multi-clock steps");
        auto stuck=trajectory.back().at("board").at("nodes").at("pad.10");
        Check(stuck.at("state_code")==15 || stuck.at("contention").get<bool>() || stuck.at("valid").get<bool>()==false,"stuck+bridge scenario shows mapped strong evidence");
        trajectory.push_back(s.Submit({{"op","step"},{"command_id","s3"},{"mode","ticks"},{"ticks","8000000"}}));
        trajectory.push_back(s.Submit(m0test::Set("key_p3",2,"release")));
        trajectory.push_back(s.Submit(m0test::Cmd("step","s4")));
        trajectory.push_back(s.Submit(m0test::Cmd("step","s5")));
        auto stopped=s.Submit(m0test::Cmd("stop","stop"));
        auto records=s.Submit({{"op","journal"}}).at("records");
        for(int repeat=0;repeat<3;++repeat){
            auto replay=vb::VirtualSession::ReplayCommitted(c,m0test::Factory(),records);
            Check(replay.at("circuit_digest")==stopped.at("digests").at("circuit") && replay.at("control_digest")==stopped.at("digests").at("control"),"SFVB source trajectory replays from zero");
        }
        auto page=s.Submit({{"op","trace.commits"},{"after_revision","0"},{"limit",128}});
        std::vector<std::pair<uint64_t,vb::LiveTransition>> events;
        for(const auto& batch:page.at("batches"))for(const auto& e:batch.at("events"))
            events.push_back({std::stoull(batch.at("revision").get<std::string>()),vb::TraceEventFromJson(e)});
        // The blind projection strips private runtime state before any encoding.
        for(auto& frame:trajectory)frame=vb::StudentProjection(frame,"opaque-p3-token");
        vb::SfvbFrameBuilder builder(42,1,LoadSignals(page));
        std::vector<vb::SfvbFrame> frames;size_t consumed=0;
        for(const auto& frame:trajectory){
            std::vector<vb::LiveTransition> slice;
            while(consumed<events.size() && events.at(consumed).first<=std::stoull(frame.at("trace").at("revision").get<std::string>()))
                {slice.push_back(events.at(consumed).second);++consumed;}
            frames.push_back(builder.Feed(frame,slice));
        }
        Check(frames.front().kind==uint16_t(vb::SfvbKind::Snapshot) && frames.size()>=10,"full snapshot then live deltas");
        std::vector<uint8_t> binary;
        for(const auto& frame:frames){auto bytes=vb::SfvbEncodeFrame(frame);binary.insert(binary.end(),bytes.begin(),bytes.end());}
        vb::SfvbStreamDecoder decode;size_t at=0;size_t index=0;bool parity=true;
        while(at<binary.size()){
            std::vector<uint8_t> slice(binary.begin()+at,binary.begin()+at+64+PayloadAt(binary,at));
            auto result=decode.Feed(slice.data(),slice.size());
            Check(result.status==vb::SfvbStreamDecoder::Status::Ok,"frame "+std::to_string(index)+" decodes");
            parity&=vb::SfvbFrameToJson(result.frame)==vb::SfvbFrameToJson(frames.at(index));
            at+=64+PayloadAt(binary,at);++index;
        }
        Check(parity && index==frames.size(),"binary stream round-trips every live frame losslessly");
        Json text=Json::array();for(const auto& frame:frames)text.push_back(vb::SfvbFrameToJson(frame));
        auto blob=text.dump();
        Check(blob.find("fault_id")==std::string::npos && blob.find("hidden-")==std::string::npos && blob.find("\"faults\"")==std::string::npos,"projected binary frames carry no private fault ids or checkpoints");
        Write(out/"native-sfvb-stream.json",{{"frames",text},{"count",frames.size()}});
    }
    Write(out/"progress.json",{{"section",4}});
    // 4. reset_glitch: a narrow pulse between clock edges still reaches eval,
    //    and it resets asynchronous logic while the clock is fully stopped.
    {
        auto c=m0test::Contract(root);UseReviewedElectrical(root,c);c.board=vb::BoardTopology::Load(root/"registry/boards/tang-nano-9k.phase1.virtual.json",c.electrical);
        c.phase1=true;c.fault_profile=vb::Phase1Faults::EmptyProfile(*c.board);
        Json bad=Json{{"id","bad-glitch"},{"enabled",true},{"visibility","student_visible"},{"target",{{"id","key_p4"},{"kind","component"}}},
            {"hook","peripheral.event"},{"effect",{{"type","reset_glitch"},{"pressed_state",0},{"duration_ticks","0"}}},{"trigger",{{"kind","permanent"},{"start_tick","0"}}}};
        Check(Error([&]{vb::Phase1Faults candidate(*c.board);Json p=candidate.Profile();p["faults"]=Json::array({bad});candidate.Load(p,0);})=="E_P3_FAULT_PARAMETERS","zero-width reset glitch rejected");
        vb::VirtualSession s(c,m0test::Factory());
        s.Submit(m0test::Set("reset_p4",2,"release"));s.Submit(m0test::Set("key_p3",13,"press"));
        s.Submit(m0test::Cmd("step","warm-1"));s.Submit(m0test::Cmd("step","warm-2"));
        auto before=s.Submit(m0test::Cmd("step","warm-3")).at("outputs");
        Check(before.at("led_n[0]").get<bool>()==false && before.at("led_n[1]").get<bool>()==false,"counter advanced before the glitch window");
        // Mid-session profile load: hold the clock, then fire the glitch.
        Json profile=c.fault_profile;
        profile["faults"]={
            Json{{"id","hold-clock"},{"enabled",true},{"visibility","teacher_only"},{"target",{{"id","clk27m"},{"kind","clock"}}},
                {"hook","clock.schedule"},{"effect",{{"type","clock_stop"},{"hold_level",0},{"resume_phase","restart_half_period"}}},
                {"trigger",{{"kind","permanent"},{"start_tick","55556"}}}},
            Json{{"id","reset-glitch-1"},{"enabled",true},{"visibility","teacher_only"},{"target",{{"id","key_p4"},{"kind","component"}}},
                {"hook","peripheral.event"},{"effect",{{"type","reset_glitch"},{"pressed_state",0},{"duration_ticks","1500"}}},
                {"trigger",{{"kind","transient"},{"start_tick","200000"},{"end_tick","2000000"}}}}};
        s.Submit({{"op","profile.load"},{"command_id","load-plan"},{"profile",profile}});
        auto glitched=s.Submit({{"op","step"},{"command_id","jump"},{"mode","ticks"},{"ticks","2000000"}});
        auto after=glitched.at("outputs");
        Check(after.at("led_n[0]").get<bool>()==true && after.at("led_n[1]").get<bool>()==true && after.at("led_n[5]").get<bool>()==true,"narrow async reset glitch cleared the counter without any clock edge");
        Write(out/"glitch-frame.json",glitched);
        Check(glitched.at("position").at("tick")=="2092593","step landed on its target with the clock held");
        auto stopped=s.Submit(m0test::Cmd("stop","stop"));auto records=s.Submit({{"op","journal"}}).at("records");
        // Fault event ledgers reset per commit; the durable journal keeps them all.
        Json pinEdges=Json::array();
        for(const auto& record:records)if(record.at("kind")=="commit")
            for(const auto& event:record.at("p1").at("faults").at("events"))if(event.at("type")=="pin.edge")pinEdges.push_back(event);
        Write(out/"glitch-events.json",pinEdges);
        Check(pinEdges.size()==2 && pinEdges.at(0).at("tick")=="200000" && pinEdges.at(1).at("tick")=="201500" && pinEdges.at(0).at("state")==13 && pinEdges.at(1).at("state")==2,"glitch expansion keeps exact press/release edges");
        for(int repeat=0;repeat<3;++repeat){auto replay=vb::VirtualSession::ReplayCommitted(c,m0test::Factory(),records);
            Check(replay.at("circuit_digest")==stopped.at("digests").at("circuit") && replay.at("control_digest")==stopped.at("digests").at("control"),"glitch session replays three times");}
    }
    Write(out/"progress.json",{{"section",5}});
    // 5. clock_divider_offset: divide ratio changes the actual clock schedule.
    {
        auto c=m0test::Contract(root);UseReviewedElectrical(root,c);c.board=vb::BoardTopology::Load(root/"registry/boards/tang-nano-9k.phase1.virtual.json",c.electrical);
        c.phase1=true;c.fault_profile=vb::Phase1Faults::EmptyProfile(*c.board);
        Json rule=Json{{"id","ratio-2"},{"enabled",true},{"visibility","teacher_only"},{"target",{{"id","clk27m"},{"kind","clock"}}},
            {"hook","clock.schedule"},{"effect",{{"type","clock_divider_offset"},{"den_offset","1"},{"resume_phase","restart_half_period"}}},
            {"trigger",{{"kind","transient"},{"start_tick","0"},{"end_tick","300000"}}}};
        c.fault_profile["faults"]={rule};
        vb::VirtualSession s(c,m0test::Factory());
        s.Submit(m0test::Set("reset_p4",2,"release"));s.Submit(m0test::Set("key_p3",13,"press"));
        auto first=s.Submit(m0test::Cmd("step","e1"));
        Check(first.at("position").at("tick")=="37037","halved clock moves the first rising edge to the doubled half period");
        auto second=s.Submit(m0test::Cmd("step","e2"));
        Check(second.at("position").at("tick")=="111111","second rising edge stays on the offset schedule");
        auto window=s.Submit({{"op","step"},{"command_id","e3"},{"mode","ticks"},{"ticks","300000"}});
        Check(window.at("status")=="applied","offset window completes");
        auto resume=s.Submit(m0test::Cmd("step","e4"));
        auto resumeTick=std::stoull(resume.at("position").at("tick").get<std::string>());
        Write(out/"clock-offset-frames.json",Json::array({first.at("position"),second.at("position"),window.at("position"),resume.at("position")}));
        Check(resumeTick-411111==18519,"after the window the base half-period schedule restarts");
        auto stopped=s.Submit(m0test::Cmd("stop","stop"));auto records=s.Submit({{"op","journal"}}).at("records");
        bool rescheduled=false;
        for(const auto& record:records)if(record.at("kind")=="commit")
            for(const auto& event:record.at("p1").at("faults").at("events"))rescheduled|=event.at("type")=="clock.rescheduled";
        Check(rescheduled,"scheduler records the divide-ratio rescheduling decision");
        Check(Error([&]{vb::Phase1Faults candidate(*c.board);auto p=candidate.Profile();Json bad=rule;bad["effect"]["den_offset"]="0";p["faults"]=Json::array({bad});candidate.Load(p,0);})=="E_P3_FAULT_PARAMETERS","zero divide offset rejected");
        Check(Error([&]{vb::Phase1Faults candidate(*c.board);auto p=candidate.Profile();Json bad=rule;bad["effect"]["den_offset"]="100000000000000000000";p["faults"]=Json::array({bad});candidate.Load(p,0);})=="E_P1_TICK_STRING","oversized divide offset rejected");
        for(int repeat=0;repeat<3;++repeat){auto replay=vb::VirtualSession::ReplayCommitted(c,m0test::Factory(),records);
            Check(replay.at("circuit_digest")==stopped.at("digests").at("circuit") && replay.at("control_digest")==stopped.at("digests").at("control"),"offset clock session replays three times");}
    }
    Write(out/"progress.json",{{"section",6}});
    // 6. Matrix keypad: ghost keys without diodes, isolation with diodes.
    {
        auto c=p3test::Contract(root,false);
        vb::VirtualSession s(c,p3test::ProbeFactory());
        Json replies=Json::array();
        auto track=[&](Json command){auto reply=s.Submit(command);replies.push_back({{"command_id",reply.value("command_id",std::string())},{"status",reply.value("status",std::string())},{"error",reply.value("error",std::string())},{"reason",reply.value("reason",std::string())},{"pending_count",reply.value("pending_count",size_t(0))}});return reply;};
        track(p3test::Set("reset_p4",2,"release"));
        track(p3test::Keys("keys",Json::array({Json::array({0,0}),Json::array({0,1}),Json::array({1,0})})));
        track(p3test::Cmd("step","apply-keys"));
        track(p3test::Set("keypad_row1",13,"scan-row1"));
        auto scanned=track(p3test::Cmd("step","scan"));
        Write(out/"ghost-replies.json",replies);
        auto keypad=scanned.at("board").at("peripherals").at("keypad0");
        Write(out/"ghost-scan.json",{{"keypad",scanned.at("board").at("peripherals").at("keypad0")},
            {"row1",scanned.at("board").at("nodes").at("pad.26")},{"row0",scanned.at("board").at("nodes").at("pad.25")},
            {"col0",scanned.at("board").at("nodes").at("pad.29")},{"col1",scanned.at("board").at("nodes").at("pad.30")},
            {"outputs",scanned.at("outputs")}});
        Check(keypad.at("closed").size()==3 && keypad.at("apparent").size()==4,"three closed keys; the merged junction also drags row0 low so both scanned rows report");
        Check(keypad.at("ghosts").size()==1 && keypad.at("ghosts").at(0)==Json::array({"1","1"}),"ghost key (1,1) emerges from the real merged network");
        Check(scanned.at("board").at("nodes").at("pad.30").at("logic")=="0" && scanned.at("board").at("nodes").at("pad.30").at("valid").get<bool>()==true,"ghost column reads a valid low through the key junction");
        Check(scanned.at("outputs").at("led_n[1]").get<bool>()==false,"DUT sees the ghost column as pressed");
        auto stopped=s.Submit(p3test::Cmd("stop","stop"));auto records=s.Submit({{"op","journal"}}).at("records");
        for(int repeat=0;repeat<3;++repeat){auto replay=vb::VirtualSession::ReplayCommitted(c,p3test::ProbeFactory(),records);
            Check(replay.at("circuit_digest")==stopped.at("digests").at("circuit") && replay.at("control_digest")==stopped.at("digests").at("control"),"ghost session replays three times");}
        // Diode variant of the same board: no back-feed, no ghost.
        auto descriptor=c.board->Descriptor();
        for(auto& comp:descriptor["components"])if(comp.at("id")=="keypad0")comp["params"]["diodes"]=true;
        auto diode=std::make_shared<const vb::BoardTopology>(descriptor,c.electrical);
        auto d=c;d.board=diode;
        vb::VirtualSession t(d,p3test::ProbeFactory());
        t.Submit(p3test::Set("reset_p4",2,"release"));
        t.Submit(p3test::Keys("keys",Json::array({Json::array({0,0}),Json::array({0,1}),Json::array({1,0})})));
        t.Submit(p3test::Cmd("step","apply-keys"));
        t.Submit(p3test::Set("keypad_row1",13,"scan-row1"));
        auto clean=t.Submit(p3test::Cmd("step","scan"));
        Write(out/"diode-scan.json",{{"keypad",clean.at("board").at("peripherals").at("keypad0")},
            {"col0",clean.at("board").at("nodes").at("pad.29")},{"col1",clean.at("board").at("nodes").at("pad.30")},
            {"outputs",clean.at("outputs")}});
        auto shielded=clean.at("board").at("peripherals").at("keypad0");
        Check(shielded.at("ghosts").empty() && shielded.at("apparent").size()==1,"anti-ghost diodes keep only the real junction");
        Check(clean.at("board").at("nodes").at("pad.30").at("logic")=="1","released row cannot back-feed the column through a diode");
        Check(clean.at("outputs").at("led_n[1]").get<bool>()==true,"DUT observes no ghost with diodes");
        Check(Error([&]{vb::Phase1Peripherals instance(*c.board,c.integration_window_ticks);instance.SetKeys("keypad0",Json::array({Json::array({4,0})}));})=="E_KEYPAD_KEYS","out-of-range key rejected");
        Check(Error([&]{vb::Phase1Peripherals instance(*c.board,c.integration_window_ticks);instance.SetKeys("missing",Json::array());})=="E_KEYPAD_TARGET","unknown keypad target rejected");
    }
    Write(out/"progress.json",{{"section",7}});
    // 7. Push-pull scanner: diode-less multi-key press short-circuits the scan
    //    network and the strict import policy pauses with CON evidence.
    {
        auto c=p3test::Contract(root,true);
        vb::VirtualSession s(c,p3test::ScannerFactory());
        s.Submit(p3test::Set("reset_p4",2,"release"));
        s.Submit(p3test::Cmd("step","free-run"));
        s.Submit(p3test::Keys("keys",Json::array({Json::array({0,0}),Json::array({0,1}),Json::array({1,0})})));
        auto blocked=s.Submit(p3test::Cmd("step","scan"));
        Check(blocked.at("status")=="blocked" && blocked.at("reason")=="UnknownInput" && blocked.at("error")=="E_UNKNOWN_INPUT","scanner contention pauses before importing contested columns");
        Check(blocked.at("guard").at("state")==46 && blocked.at("guard").at("drivers").size()>=2,"guard keeps the folded CON state and both fighting drivers");
        auto releaseReply=s.Submit(p3test::Keys("keys-release",Json::array()));
        Write(out/"scanner-release.json",{{"status",releaseReply.value("status",std::string())},{"error",releaseReply.value("error",std::string())},{"pending_count",releaseReply.value("pending_count",size_t(0))}});
        auto recovered=s.Submit(p3test::Cmd("step","recover"));
        Write(out/"scanner-recover.json",{{"status",recovered.value("status",std::string())},{"reason",recovered.value("reason",std::string())},{"error",recovered.value("error",std::string())},{"guard",recovered.value("guard",Json::object())}});
        Check(recovered.at("reason")=="StepComplete","releasing the keys unblocks the strict guard");
        auto stopped=s.Submit(p3test::Cmd("stop","stop"));auto records=s.Submit({{"op","journal"}}).at("records");
        for(int repeat=0;repeat<3;++repeat){auto replay=vb::VirtualSession::ReplayCommitted(c,p3test::ScannerFactory(),records);
            Check(replay.at("circuit_digest")==stopped.at("digests").at("circuit") && replay.at("control_digest")==stopped.at("digests").at("control"),"contention session replays three times");}
    }
    report={{"status","passed"},{"checks",checks}};
    Write(out/"native.json",report);std::cout<<report.dump()<<std::endl;return 0;
}
// -------- encode: build SFVB stream + source JSON from a real trajectory ----
int RunEncode(const fs::path& input,const fs::path& stream,const fs::path& frames,const fs::path& decoded){
    auto trajectory=Read(input);
    std::vector<vb::LiveSignal> signals;for(const auto& s:trajectory.at("signals"))signals.push_back({s.at("id"),s.at("name"),s.at("width"),s.at("kind")});
    vb::SfvbFrameBuilder builder(uint32_t(std::stoull(trajectory.at("run_handle").get<std::string>())),
        uint32_t(std::stoull(trajectory.at("generation").get<std::string>())),signals);
    std::vector<vb::SfvbFrame> built;
    // Each run contributes its own frames and trace batches; a new run id
    // forces a fresh snapshot, exercising the real mid-stream re-baseline.
    for(const auto& run:trajectory.at("runs")){
        size_t consumed=0;
        for(const auto& frame:run.at("frames")){
        std::vector<vb::LiveTransition> slice;
        const auto& batches=run.at("trace_batches");
        while(consumed<batches.size() && std::stoull(batches.at(consumed).at("revision").get<std::string>())<=std::stoull(frame.at("trace").at("revision").get<std::string>())){
            for(const auto& e:batches.at(consumed).at("events"))slice.push_back(vb::TraceEventFromJson(e));
            ++consumed;
        }
            built.push_back(builder.Feed(frame,slice));
        }
    }
    Json source=Json::array();std::vector<uint8_t> binary;
    for(const auto& frame:built){source.push_back(vb::SfvbFrameToJson(frame));auto bytes=vb::SfvbEncodeFrame(frame);binary.insert(binary.end(),bytes.begin(),bytes.end());}
    SaveBytes(stream,binary);
    Write(frames,{{"schema","sigflow.sfvb-parity.v1"},{"count",built.size()},{"frames",source}});
    vb::SfvbStreamDecoder decoder;size_t at=0;Json round=Json::array();bool parity=true;
    while(at<binary.size()){
        std::vector<uint8_t> slice(binary.begin()+at,binary.begin()+at+64+PayloadAt(binary,at));
        auto result=decoder.Feed(slice.data(),slice.size());
        if(result.status!=vb::SfvbStreamDecoder::Status::Ok)throw std::runtime_error("E_SFVB_STREAM: "+result.error);
        parity&=vb::SfvbFrameToJson(result.frame)==source.at(round.size());
        round.push_back(vb::SfvbFrameToJson(result.frame));at+=64+PayloadAt(binary,at);
    }
    if(!parity || round.size()!=built.size())throw std::runtime_error("E_SFVB_PARITY");
    Write(decoded,{{"schema","sigflow.sfvb-parity.v1"},{"count",round.size()},{"frames",round}});
    Json summary={{"status","passed"},{"frames",built.size()}};
    Write(fs::path(stream).parent_path()/"encode-summary.json",summary);std::cout<<summary.dump()<<std::endl;return 0;
}
// -------- decode: stream decode with full continuity enforcement ----------
int RunDecode(const fs::path& input,const fs::path& output){
    auto binary=LoadBytes(input);
    vb::SfvbStreamDecoder decoder;size_t at=0;Json frames=Json::array();
    while(at+vb::SfvbHeaderBytes<=binary.size()){
        std::vector<uint8_t> slice(binary.begin()+at,binary.begin()+at+64+PayloadAt(binary,at));
        auto result=decoder.Feed(slice.data(),slice.size());
        if(result.status!=vb::SfvbStreamDecoder::Status::Ok){Write(output,{{"status","rejected"},{"error",result.error},{"decoded_frames",frames.size()}});std::cout<<Json{{"status","rejected"},{"error",result.error}}.dump()<<std::endl;return 1;}
        frames.push_back(vb::SfvbFrameToJson(result.frame));at+=64+PayloadAt(binary,at);
    }
    if(at!=binary.size())throw std::runtime_error("E_SFVB_TRAILING_BYTES");
    Write(output,{{"schema","sigflow.sfvb-parity.v1"},{"count",frames.size()},{"frames",frames}});
    std::cout<<Json{{"status","passed"},{"frames",frames.size()}}.dump()<<std::endl;return 0;
}
// -------- decode-broken: continuity rules must fire on real corruptions ----
int RunDecodeBroken(const fs::path& input,const std::string& mode,const fs::path& output){
    auto binary=LoadBytes(input);
    std::vector<std::vector<uint8_t>> frames;size_t at=0;
    while(at+vb::SfvbHeaderBytes<=binary.size()){frames.push_back(std::vector<uint8_t>(binary.begin()+at,binary.begin()+at+64+PayloadAt(binary,at)));at+=64+PayloadAt(binary,at);}
    vb::SfvbStreamDecoder decoder;
    auto r0=decoder.Feed(frames.at(0).data(),frames.at(0).size());
    if(r0.status!=vb::SfvbStreamDecoder::Status::Ok)throw std::runtime_error("E_SFVB_BASELINE");
    std::string observed;
    if(mode=="gap")observed=frames.size()>2?decoder.Feed(frames.at(2).data(),frames.at(2).size()).error:std::string();
    else{
        auto frame=vb::SfvbDecodeFrame(frames.at(1).data(),frames.at(1).size());
        if(mode=="baseline")frame.base_seq+=1;
        else if(mode=="stale")frame.generation+=1;
        else throw std::runtime_error("E_CORRUPTION_MODE");
        auto bytes=vb::SfvbEncodeFrame(frame);
        observed=decoder.Feed(bytes.data(),bytes.size()).error;
    }
    static const std::map<std::string,std::string> expected{{"gap","E_SFVB_SEQ_GAP"},{"baseline","E_SFVB_BASELINE_LOST"},{"stale","E_SFVB_GENERATION_STALE"}};
    bool passed=observed==expected.at(mode);
    Write(output,{{"mode",mode},{"expected",expected.at(mode)},{"observed",observed},{"status",passed?"passed":"failed"}});
    std::cout<<Json{{"mode",mode},{"observed",observed},{"status",passed?"passed":"failed"}}.dump()<<std::endl;return passed?0:1;
}
int main(int argc,char** argv){
    if(argc<4)return 2;
    try{
        std::string mode=argv[1];
        if(mode=="native" && argc==4)return RunNative(eda::platform::PathFromUtf8(argv[2]),eda::platform::PathFromUtf8(argv[3]));
        if(mode=="encode" && argc==6)return RunEncode(eda::platform::PathFromUtf8(argv[2]),eda::platform::PathFromUtf8(argv[3]),eda::platform::PathFromUtf8(argv[4]),eda::platform::PathFromUtf8(argv[5]));
        if(mode=="decode" && argc==4)return RunDecode(eda::platform::PathFromUtf8(argv[2]),eda::platform::PathFromUtf8(argv[3]));
        if(mode=="decode-broken" && argc==5)return RunDecodeBroken(eda::platform::PathFromUtf8(argv[2]),argv[3],eda::platform::PathFromUtf8(argv[4]));
    }catch(const std::exception& e){
        Json failure={{"status","failed"},{"error",e.what()}};
        std::cerr<<failure.dump()<<std::endl;
        Write(eda::platform::PathFromUtf8(argv[argc-1]),failure);
        return 1;
    }
    return 2;
}
