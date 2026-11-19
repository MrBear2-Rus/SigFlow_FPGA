#include "wave/WaveCompareSession.h"
#include "virtual/VirtualCaptureArtifacts.h"
#include "debug/CaptureDecoder.h"
#include "eda-platform/Platform.h"
#include <fstream>
#include <iostream>
using eda::Json;namespace fs=std::filesystem;
Json Read(const fs::path& p){std::ifstream f(p);Json j;f>>j;return j;}
int main(int argc,char** argv){if(argc!=5)return 2;try{auto expected=Read(eda::platform::PathFromUtf8(argv[1])),actual=Read(eda::platform::PathFromUtf8(argv[2]));auto folder=eda::platform::PathFromUtf8(argv[3]);std::string wanted=argv[4];auto result=sigflow::wave::WaveCompareSession::CompareSampledCaptures(expected,actual);if(result.at("status")!=wanted)throw std::runtime_error("E_EXPECTED_COMPARISON: "+result.dump());auto files=sigflow::virtual_lab::ExportVirtualCapture(actual,folder);uint32_t depth,width;uint16_t start;std::vector<uint32_t> samples;std::string error;if(!sigflow::debug::LoadCaptureRaw(files.at("raw"),depth,width,start,samples,error) || samples.size()!=actual.at("samples").size())throw std::runtime_error("E_RAW_ROUNDTRIP");for(size_t i=0;i<samples.size();++i)if(samples[i]!=actual.at("samples")[i].at("value"))throw std::runtime_error("E_RAW_SAMPLE");result["artifacts"]=files;std::ofstream(folder/"comparison.json")<<result.dump(2);std::cout<<result.dump()<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
