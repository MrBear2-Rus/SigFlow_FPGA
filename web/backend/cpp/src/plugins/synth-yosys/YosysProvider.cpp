#include "YosysProvider.h"
#include "YosysScriptGenerator.h"
#include "eda-core/JobRuntime.h"
#include "eda-core/JobPaths.h"
#include <eda/api/process.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace fs=std::filesystem;
namespace eda::synth {
Json YosysProvider::paramsSchema() const {
    return {{"type","object"},{"required",{"top_module","source_files"}},
        {"properties",{{"top_module",{{"type","string"}}},{"source_files",{{"type","array"}}},
        {"strategy",{{"type","string"}}},{"yosys_family",{{"type","string"}}},
        {"output_json",{{"type","string"}}},{"yosys_path",{{"type","string"}}},
        {"target_profile",{{"type","string"}}},{"target_version",{{"type","string"}}}}}};
}
Json YosysProvider::resultSchema() const {return {{"type","object"},{"properties",{{"netlist",{{"type","string"}}}}}};}
Error YosysProvider::startJob(const JobRequest& request,JobContext& ctx) {
    try {
        const auto& params=request.params;
        auto str=[&](const char* name,const std::string& fallback=std::string()) {
            if(!params.contains(name))return fallback;
            if(!params.at(name).is_string())throw std::invalid_argument(std::string(name)+" must be a string");
            return params.at(name).get<std::string>();
        };
        YosysScriptRequest script; script.topModule=str("top_module");
        script.yosysFamily=str("yosys_family","gw1n");
        auto strategy=str("strategy","baseline");
        if(!ParseStrategy(strategy,script.strategy))throw std::invalid_argument("unknown synthesis strategy");
        script.targetProfileId=str("target_profile");script.targetProfileVersion=str("target_version");
        if(!params.contains("source_files")||!params.at("source_files").is_array()||params.at("source_files").empty())
            throw std::invalid_argument("source_files must be a nonempty array");
        for(const auto& file:params.at("source_files")) {
            if(!file.is_string()||file.get<std::string>().empty())throw std::invalid_argument("source_files entries must be nonempty strings");
            script.sourceFiles.push_back(web::CheckedInput(ctx,file.get<std::string>()).string());
        }
        auto out=str("output_json");
        fs::path output=out.empty()?ctx.jobDir()/"artifacts/top.json":fs::path(out);
        output=web::CheckedPath(ctx.jobDir()/"artifacts",output,false);
        if(output==ctx.jobDir()/"artifacts")throw std::invalid_argument("output_json must name a file");
        script.outputJsonPath=output.string();
        auto generated=YosysScriptGenerator().Generate(script);
        if(!generated.success)throw std::invalid_argument(generated.errorMessage);
        ToolQuery query;query.name="yosys";query.configuredPath=str("yosys_path");query.environmentVariables={"SIGFLOW_YOSYS"};
        auto tool=web::ResolveExecutionTool(ctx,query);
        if(!tool.found)return {ErrorCode::NotFound,"yosys not found: "+tool.reason,""};
        auto scriptPath=web::CheckedPath(ctx.jobDir(),"run_yosys.ys",false);
        fs::create_directories(output.parent_path());
        {std::ofstream file(scriptPath,std::ios::binary|std::ios::trunc);file<<generated.script;file.close();if(!file)throw std::runtime_error("cannot write Yosys script");}
        ctx.log("Yosys script prepared\n",false);
        ProcessSpec spec;spec.executable=tool.path;spec.arguments={"-s",scriptPath.string()};spec.workingDirectory=ctx.jobDir();
        auto result=ctx.processHost().Run(spec,[&](const auto& chunk,bool error){ctx.log(chunk,error);});
        ctx.emitMetric(Json{{"exit_code",result.exitCode}});
        if(result.outcome==ProcessOutcome::Cancelled)return {ErrorCode::Cancelled,"synthesis cancelled",""};
        if(result.outcome==ProcessOutcome::TimedOut)return {ErrorCode::TimedOut,"synthesis timed out",""};
        if(!result.started)return {ErrorCode::Crashed,result.errorMessage,""};
        if(result.exitCode!=0)return {ErrorCode::Internal,"yosys exited with code "+std::to_string(result.exitCode),""};
        output=web::CheckedPath(ctx.jobDir()/"artifacts",output,true);
        if(!fs::is_regular_file(output)||fs::file_size(output)<32)throw std::runtime_error("missing or too small Yosys JSON netlist");
        std::ifstream stream(output,std::ios::binary);Json netlist;stream>>netlist;
        if(!stream||!netlist.is_object()||!netlist.contains("modules")||!netlist["modules"].is_object()||
           !netlist["modules"].contains(script.topModule)||!netlist["modules"][script.topModule].is_object())
            throw std::runtime_error("netlist missing expected top module");
        const auto& top=netlist["modules"][script.topModule];
        for(const char* field:{"ports","cells","netnames"})
            if(!top.contains(field)||!top[field].is_object())throw std::runtime_error(std::string("netlist missing object: ")+field);
        ctx.emitMetric(Json{{"ports",top["ports"].size()},{"cells",top["cells"].size()},{"netnames",top["netnames"].size()}});
        Artifact artifact;artifact.id="netlist";artifact.path=output;artifact.schema="eda.netlist.yosys-json.v1";artifact.role="primary";
        ctx.registerArtifact(artifact);ctx.progress(100,"synthesis complete");return Error::Ok();
    } catch(const std::invalid_argument& error) {return {ErrorCode::InvalidArgument,error.what(),""};}
      catch(const std::exception& error) {return {ErrorCode::Internal,error.what(),""};}
}
}
