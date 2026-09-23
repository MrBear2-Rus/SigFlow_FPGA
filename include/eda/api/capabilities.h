#pragma once

#include <string>
#include <utility>
#include <vector>

#include "IPluginInteraction.h"

namespace eda {

// 类型化能力方法：进程内插件覆写 invoke 即可获得 JSON-RPC 语义，
// 类型化方法默认经 invoke + toJson/fromJson（ADL）自动翻译。
#define EDA_TYPED_METHOD(method, MethodParams, MethodResult)                          \
    virtual void method(const MethodParams& p, Callback<Error, MethodResult> cb) {    \
        invoke(MethodCall{#method, toJson(p), {}},                                    \
               [cb = std::move(cb)](Error e, Json r) {                                \
                   if (static_cast<bool>(e)) {                                        \
                       cb(e, MethodResult{});                                         \
                       return;                                                        \
                   }                                                                  \
                   cb(Error::Ok(), fromJson<MethodResult>(r));                        \
               });                                                                    \
    }                                                                                 \
    static constexpr const char* k##method = #method

struct SynthParams {
    std::string topModule;
    std::vector<std::string> sourceFiles;
    std::string strategy = "baseline";
};

struct SynthResult {
    std::string jobId;
};

inline Json toJson(const SynthParams& p) {
    return Json{{"top_module", p.topModule},
                {"source_files", p.sourceFiles},
                {"strategy", p.strategy}};
}

inline Json toJson(const SynthResult& r) {
    return Json{{"job_id", r.jobId}};
}

template <>
inline SynthResult fromJson<SynthResult>(const Json& j) {
    return SynthResult{j.value("job_id", std::string{})};
}

class ISynthesizer : public IPluginInteraction {
public:
    virtual std::string synthesizerId() const = 0;
    EDA_TYPED_METHOD(synth_run, SynthParams, SynthResult);
};

struct PnrParams {
    std::string topModule;
    std::string netlist;   // 输入 Yosys JSON
    std::string device;
    std::string family;
    std::string cst;
    std::string output;    // 输出 .pnr.json
};

struct PnrResult {
    std::string pnrJson;
};

inline Json toJson(const PnrParams& p) {
    return Json{{"top_module", p.topModule},
                {"netlist", p.netlist},
                {"device", p.device},
                {"family", p.family},
                {"cst", p.cst},
                {"output", p.output}};
}

inline Json toJson(const PnrResult& r) { return Json{{"pnr_json", r.pnrJson}}; }

template <>
inline PnrResult fromJson<PnrResult>(const Json& j) {
    return PnrResult{j.value("pnr_json", std::string{})};
}

class IPlaceAndRouter : public IPluginInteraction {
public:
    virtual std::string pnrId() const = 0;
    EDA_TYPED_METHOD(pnr_run, PnrParams, PnrResult);
};

struct PackParams {
    std::string pnrJson;   // 输入 .pnr.json
    std::string device;    // 如 GW1N-9C
    std::string output;    // 输出 .fs
};

struct PackResult {
    std::string bitstream;
};

inline Json toJson(const PackParams& p) {
    return Json{{"pnr_json", p.pnrJson}, {"device", p.device}, {"output", p.output}};
}

inline Json toJson(const PackResult& r) { return Json{{"bitstream", r.bitstream}}; }

template <>
inline PackResult fromJson<PackResult>(const Json& j) {
    return PackResult{j.value("bitstream", std::string{})};
}

class IPacker : public IPluginInteraction {
public:
    virtual std::string packerId() const = 0;
    EDA_TYPED_METHOD(pack_run, PackParams, PackResult);
};

struct FlashParams {
    std::string bitstream;
    std::string board;
};

struct FlashResult {
    bool programmed = false;
};

inline Json toJson(const FlashParams& p) {
    return Json{{"bitstream", p.bitstream}, {"board", p.board}};
}

inline Json toJson(const FlashResult& r) { return Json{{"programmed", r.programmed}}; }

template <>
inline FlashResult fromJson<FlashResult>(const Json& j) {
    return FlashResult{j.value("programmed", false)};
}

class IProgrammer : public IPluginInteraction {
public:
    virtual std::string programmerId() const = 0;
    virtual bool requiresConfirm() const = 0;
    EDA_TYPED_METHOD(flash_run, FlashParams, FlashResult);
};

struct SimBuildParams {
    std::string topModule;
    std::vector<std::string> sourceFiles;
    std::string testbench;  // 可选
    std::string outDir;
};

struct SimBuildResult {
    std::string executable;
};

inline Json toJson(const SimBuildParams& p) {
    return Json{{"top_module", p.topModule},
                {"source_files", p.sourceFiles},
                {"testbench", p.testbench},
                {"out_dir", p.outDir}};
}

inline Json toJson(const SimBuildResult& r) { return Json{{"executable", r.executable}}; }

template <>
inline SimBuildResult fromJson<SimBuildResult>(const Json& j) {
    return SimBuildResult{j.value("executable", std::string{})};
}

class ISimulator : public IPluginInteraction {
public:
    virtual std::string simulatorId() const = 0;
    EDA_TYPED_METHOD(sim_build, SimBuildParams, SimBuildResult);
};

} // namespace eda
