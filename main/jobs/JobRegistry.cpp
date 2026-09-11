#include "JobService.h"
#include "ToolJobs.h"

#include <utility>

namespace {

// DS-02 参数 schema：AI 生成参数与 UI 表单的唯一依据（字段名/类型/必填/枚举）。
Json::Value Schema(const std::vector<std::pair<wxString, wxString>>& properties,
                   const std::vector<wxString>& required)
{
    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    Json::Value fields(Json::objectValue);
    for (const auto& property : properties) {
        Json::Value field(Json::objectValue);
        field["type"] = property.second.ToStdString();
        fields[property.first.ToStdString()] = field;
    }
    schema["properties"] = fields;
    Json::Value requiredNames(Json::arrayValue);
    for (const wxString& name : required) requiredNames.append(name.ToStdString());
    schema["required"] = requiredNames;
    return schema;
}

JobToolDescriptor Descriptor(ToolJobType type, const wxString& name, const wxString& description,
                             const Json::Value& schema, std::size_t maxConcurrent,
                             JobRunHandler handler)
{
    JobToolDescriptor descriptor;
    descriptor.type = type;
    descriptor.name = name;
    descriptor.description = description;
    descriptor.inputSchema = schema;
    descriptor.maxConcurrent = maxConcurrent;
    descriptor.defaultTimeoutSeconds = DefaultJobTimeoutSeconds(type);
    descriptor.handler = std::move(handler);
    return descriptor;
}

} // namespace

int DefaultJobTimeoutSeconds(ToolJobType type)
{
    switch (type) {
    // 仿真编译 + 运行、Yosys 综合、nextpnr 布局布线跑一两分钟都正常，默认给足 10 分钟。
    case ToolJobType::Simulation: return 600;
    case ToolJobType::Synthesis: return 600;
    case ToolJobType::PnR: return 600;
    case ToolJobType::Pack: return 300;
    case ToolJobType::Flash: return 300;
    }
    return 600;
}

JobServiceRegistry CreateDefaultJobServiceRegistry()
{
    JobServiceRegistry registry;
    wxString ignored;

    // 仿真：既支持外部进程，也支持进程内 SimulationEngine 运行体。
    Json::Value simulationSchema = Schema({
        { "top_module", "string" }, { "output_vcd", "string" },
        { "source_files", "array" }, { "executable", "string" },
        { "timeout_seconds", "integer" },
    }, { "top_module", "output_vcd", "source_files" });
    registry.Register(Descriptor(
        ToolJobType::Simulation, "sim.run",
        "Run a simulation and publish a VCD artifact (external tool or in-process engine).",
        simulationSchema, 1,
        [](const ToolJob& job, const JobExecutionOptions& options, JobReport& report,
           wxString& errorMessage) {
            SimulationJobRequest request;
            request.projectPath = job.request.projectPath;
            request.topModule = wxString::FromUTF8(
                job.request.parameters.get("top_module", "").asString());
            request.outputVcdPath = wxString::FromUTF8(
                job.request.parameters.get("output_vcd", "").asString());
            request.executable = wxString::FromUTF8(
                job.request.parameters.get("executable", "").asString());
            request.workingDirectory = wxString::FromUTF8(
                job.request.parameters.get("working_directory", "").asString());
            request.timeoutSeconds = job.request.parameters.get("timeout_seconds", 0).asInt();
            for (const Json::Value& source : job.request.parameters["source_files"]) {
                request.sourceFiles.push_back(wxString::FromUTF8(source.asString()));
            }
            for (const Json::Value& argument : job.request.parameters["arguments"]) {
                request.arguments.push_back(wxString::FromUTF8(argument.asString()));
            }
            return SimulationJob().Execute(request, job, options, report, errorMessage);
        }), ignored);

    // 综合（Yosys）：复用既有日志解析器与产物校验器，报告错误可直接喂给 AI。
    Json::Value synthesisSchema = Schema({
        { "top_module", "string" }, { "output_json", "string" },
        { "executable", "string" }, { "script_path", "string" },
        { "source_files", "array" }, { "timeout_seconds", "integer" },
    }, { "top_module", "output_json", "executable" });
    registry.Register(Descriptor(
        ToolJobType::Synthesis, "fpga.synthesis",
        "Synthesize RTL with Yosys and publish a validated netlist artifact.",
        synthesisSchema, 1,
        [](const ToolJob& job, const JobExecutionOptions& options, JobReport& report,
           wxString& errorMessage) {
            SynthJobRequest request;
            request.projectPath = job.request.projectPath;
            request.topModule = wxString::FromUTF8(
                job.request.parameters.get("top_module", "").asString());
            request.outputJsonPath = wxString::FromUTF8(
                job.request.parameters.get("output_json", "").asString());
            request.executable = wxString::FromUTF8(
                job.request.parameters.get("executable", "").asString());
            request.scriptPath = wxString::FromUTF8(
                job.request.parameters.get("script_path", "").asString());
            request.workingDirectory = wxString::FromUTF8(
                job.request.parameters.get("working_directory", "").asString());
            request.timeoutSeconds = job.request.parameters.get("timeout_seconds", 0).asInt();
            for (const Json::Value& source : job.request.parameters["source_files"]) {
                request.sourceFiles.push_back(wxString::FromUTF8(source.asString()));
            }
            for (const Json::Value& argument : job.request.parameters["arguments"]) {
                request.arguments.push_back(wxString::FromUTF8(argument.asString()));
            }
            return SynthJob().Execute(request, job, options, report, errorMessage);
        }), ignored);

    // 布局布线（nextpnr）：报告 fmax 与结构化错误。
    Json::Value pnrSchema = Schema({
        { "top_module", "string" }, { "output_json", "string" },
        { "executable", "string" }, { "timeout_seconds", "integer" },
    }, { "top_module", "output_json", "executable" });
    registry.Register(Descriptor(
        ToolJobType::PnR, "fpga.pnr",
        "Place and route a synthesized netlist with nextpnr.",
        pnrSchema, 1,
        [](const ToolJob& job, const JobExecutionOptions& options, JobReport& report,
           wxString& errorMessage) {
            PnRJobRequest request;
            request.projectPath = job.request.projectPath;
            request.topModule = wxString::FromUTF8(
                job.request.parameters.get("top_module", "").asString());
            request.outputJsonPath = wxString::FromUTF8(
                job.request.parameters.get("output_json", "").asString());
            request.executable = wxString::FromUTF8(
                job.request.parameters.get("executable", "").asString());
            request.workingDirectory = wxString::FromUTF8(
                job.request.parameters.get("working_directory", "").asString());
            request.timeoutSeconds = job.request.parameters.get("timeout_seconds", 0).asInt();
            for (const Json::Value& argument : job.request.parameters["arguments"]) {
                request.arguments.push_back(wxString::FromUTF8(argument.asString()));
            }
            return PnRJob().Execute(request, job, options, report, errorMessage);
        }), ignored);

    // 打包（Apicula gowin_pack）：复用 FpgaPackService 的 SHA-256 记账。
    Json::Value packSchema = Schema({
        { "pnr_json", "string" }, { "bitstream", "string" },
        { "executable", "string" }, { "device", "string" },
        { "timeout_seconds", "integer" },
    }, { "pnr_json", "bitstream", "executable" });
    registry.Register(Descriptor(
        ToolJobType::Pack, "fpga.pack",
        "Pack a placed FPGA netlist into a bitstream.",
        packSchema, 1,
        [](const ToolJob& job, const JobExecutionOptions& options, JobReport& report,
           wxString& errorMessage) {
            PackJobRequest request;
            request.projectPath = job.request.projectPath;
            request.pnrJsonPath = wxString::FromUTF8(
                job.request.parameters.get("pnr_json", "").asString());
            request.bitstreamPath = wxString::FromUTF8(
                job.request.parameters.get("bitstream", "").asString());
            request.executable = wxString::FromUTF8(
                job.request.parameters.get("executable", "").asString());
            request.device = wxString::FromUTF8(
                job.request.parameters.get("device", "").asString());
            request.workingDirectory = wxString::FromUTF8(
                job.request.parameters.get("working_directory", "").asString());
            request.timeoutSeconds = job.request.parameters.get("timeout_seconds", 0).asInt();
            for (const Json::Value& argument : job.request.parameters["arguments"]) {
                request.arguments.push_back(wxString::FromUTF8(argument.asString()));
            }
            return PackJob().Execute(request, job, options, report, errorMessage);
        }), ignored);

    // 烧录（openFPGALoader）：require_confirm 门控。
    Json::Value flashSchema = Schema({
        { "bitstream", "string" }, { "executable", "string" },
        { "require_confirm", "boolean" }, { "timeout_seconds", "integer" },
    }, { "bitstream", "executable" });
    registry.Register(Descriptor(
        ToolJobType::Flash, "fpga.flash",
        "Program a validated FPGA bitstream after user confirmation.",
        flashSchema, 1,
        [](const ToolJob& job, const JobExecutionOptions& options, JobReport& report,
           wxString& errorMessage) {
            FlashJobRequest request;
            request.projectPath = job.request.projectPath;
            request.bitstreamPath = wxString::FromUTF8(
                job.request.parameters.get("bitstream", "").asString());
            request.executable = wxString::FromUTF8(
                job.request.parameters.get("executable", "").asString());
            request.workingDirectory = wxString::FromUTF8(
                job.request.parameters.get("working_directory", "").asString());
            request.timeoutSeconds = job.request.parameters.get("timeout_seconds", 0).asInt();
            request.requireConfirm = job.request.parameters.get("require_confirm", true).asBool();
            for (const Json::Value& argument : job.request.parameters["arguments"]) {
                request.arguments.push_back(wxString::FromUTF8(argument.asString()));
            }
            return FlashJob().Execute(request, job, options, report, errorMessage);
        }), ignored);

    return registry;
}
