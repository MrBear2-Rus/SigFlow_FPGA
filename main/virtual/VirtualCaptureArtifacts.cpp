#include "VirtualCaptureArtifacts.h"
#include "debug/CaptureDecoder.h"
#include "debug/DebugProtocol.h"
#include "eda-platform/Platform.h"
#include <fstream>
namespace sigflow::virtual_lab {
eda::Json ExportVirtualCapture(const eda::Json& capture,const std::filesystem::path& folder){
    if(capture.at("armed")==true || capture.at("samples").empty())throw std::runtime_error("E_CAPTURE_INCOMPLETE");std::filesystem::create_directories(folder);std::vector<uint32_t> samples,valid;std::vector<debug::CaptureProbe> probes;unsigned bit=0;for(const auto& t:capture.at("targets"))probes.push_back({"p"+std::to_string(bit),t.get<std::string>(),1,bit++});for(const auto& s:capture.at("samples")){samples.push_back(s.at("value"));valid.push_back(s.at("valid_mask"));}
    debug::CaptureVcdMeta meta;std::string error;auto raw=folder/"capture.raw",vcd=folder/"capture.vcd";if(!debug::SaveCaptureRaw(eda::platform::PathToUtf8(raw),samples.size(),32,0,samples,error))throw std::runtime_error(error);
    debug::CaptureTimebase timebase;timebase.sampleClockHz=std::stoull(capture.at("sample_clock_hz").get<std::string>());timebase.decimation=capture.at("decimation");timebase.triggerObserved=capture.at("triggered");timebase.validMasks=valid;
    for(const auto& s:capture.at("samples"))timebase.sampleTicks.push_back(std::stoull(s.at("tick").get<std::string>()));
    if(!debug::DecodeCaptureToVcd(samples,samples.size(),capture.at("trigger_index"),probes,eda::platform::PathToUtf8(vcd),meta,error,timebase))throw std::runtime_error(error);auto data=capture;data["vcd_time_origin_tick"]=capture.at("samples").at(0).at("tick");data["vcd_quantization"]="exact_recorded_ticks_relative_to_first_1ps";std::ofstream(folder/"capture.metadata.json")<<data.dump(2)<<'\n';return {{"raw",eda::platform::PathToUtf8(raw)},{"vcd",eda::platform::PathToUtf8(vcd)},{"metadata",eda::platform::PathToUtf8(folder/"capture.metadata.json")}};
}
}
