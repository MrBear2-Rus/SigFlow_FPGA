// fpga_flow_probe.cpp — 无 GUI 的 FPGA 工具链探针
//
// 目的：**不启动主程序**，直接调用 SigFlow 内部的 FPGA 代码路径，
// 在 Linux 上把"校验 → 生成脚本 → 跑 Yosys → 校验产物 → 跑 nextpnr"整条链路跑一遍。
//
// 背景：本仓库 external/fpga-tools/runtime 下随包分发的工具全是 Windows PE 二进制
// （yosys.exe / nextpnr-himbaechel.exe），Linux 上无法执行；系统里也没有安装。
// 因此本探针默认使用**桩工具**（bash 脚本）替换 yosys/nextpnr：
//   * 桩只负责"生成结构合法的产物 + 打印仿真风格的日志"；
//   * 真正的被测对象是 SigFlow 自己的代码：路径拼接、脚本生成、进程启动、
//     输出解码、日志解析、产物校验、状态迁移。
// 一旦 Linux 上有了真实工具，用 `<toolDir>` 参数指过去即可跑真链路。
//
// 用法：fpga_flow_probe <workDir> [toolDir] [step]
//   step ∈ prep | preflight | script | synth | pnr | all（默认 all）

#include "FpgaYosysRuntime.h"
#include "FpgaYosysScriptGenerator.h"
#include "FpgaYosysExecutor.h"
#include "fpga/ArtifactValidator.h"
#include "fpga/FpgaYosysLogParser.h"
#include "fpga/FpgaYosysReport.h"
#include "fpga/NextpnrExecutor.h"
#include "fpga/NextpnrLogParser.h"
#include "fpga/CstValidator.h"
#include "jobs/PlatformProcess.h"
#include "platform/PlatformPaths.h"

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/init.h>
#include <wx/log.h>
#include <wx/string.h>
#include <wx/utils.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

void Section(const char* title)
{
    std::printf("\n==================== %s ====================\n", title);
    std::fflush(stdout);
}

void Note(const wxString& text)
{
    std::printf("%s\n", text.ToUTF8().data());
    std::fflush(stdout);
}

bool WriteFileUtf8(const wxString& path, const std::string& content)
{
    wxFile file(path, wxFile::write);
    if (!file.IsOpened()) return false;
    return file.Write(content.data(), content.size()) ==
           static_cast<wxFileOffset>(content.size());
}

std::string ReadFileUtf8(const wxString& path)
{
    wxFile file(path, wxFile::read);
    if (!file.IsOpened()) return {};
    wxString content;
    if (!file.ReadAll(&content)) return {};
    const wxScopedCharBuffer utf8 = content.ToUTF8();
    return std::string(utf8.data() ? utf8.data() : "", utf8.length());
}

// 同步等待异步执行器（YosysExecutor/NextpnrExecutor 都在 worker 线程里回调）。
template <typename Executor>
typename Executor::Result RunSync(Executor& executor,
                                  const wxString& executable,
                                  const std::vector<wxString>& args,
                                  const typename Executor::Config& config,
                                  bool& started)
{
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    typename Executor::Result captured;

    started = executor.Execute(
        executable, args, config,
        [](typename Executor::OutputStream, const wxString&) {},
        [&](const typename Executor::Result& result) {
            std::lock_guard<std::mutex> lock(mutex);
            captured = result;
            finished = true;
            done.notify_all();
        });

    if (!started) {
        return captured;
    }
    std::unique_lock<std::mutex> lock(mutex);
    if (!done.wait_for(lock, std::chrono::seconds(120), [&] { return finished; })) {
        std::printf("[probe] TIMEOUT waiting for executor completion\n");
    }
    return captured;
}

// ---------------------------------------------------------------- 桩工具
bool WriteStubTools(const wxString& toolDir, wxString& yosysPath, wxString& nextpnrPath)
{
    if (!wxFileName::Mkdir(toolDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL) &&
        !wxDir::Exists(toolDir)) {
        return false;
    }

    yosysPath = sigflow::platform::JoinPath(toolDir, "yosys");
    nextpnrPath = sigflow::platform::JoinPath(toolDir, "nextpnr-himbaechel");

    // 桩 yosys：从 -s 脚本里取出 top 与 write_json 目标，产出最小但结构合法的 JSON。
    const std::string yosysStub = R"SH(#!/usr/bin/env bash
set -u
script=""
while [ $# -gt 0 ]; do
  case "$1" in
    -s) script="$2"; shift 2;;
    -p) shift 2;;
    *) shift;;
  esac
done

echo " /----------------------------------------------------------------------------\\\\"
echo " |  Yosys 0.38+stub (Linux probe)                                             |"
echo " \\\\----------------------------------------------------------------------------/"
echo ""
echo "1. Executing Verilog-2001 parser."
echo "2. Executing AST frontend in derive mode using pre-parsed AST for module \`top'."
echo "3. Executing HIERARCHY pass (managing design hierarchy)."
echo "   Top module:  \\top"
echo "4. Executing PROC pass (convert processes to netlists)."
echo "5. Executing OPT_EXPR pass (perform const folding)."
echo "6. Executing TECHMAP pass (map to technology primitives)."
echo "7. Executing SYNTH_GOWIN pass (synthesis for Gowin FPGAs)."
echo ""
echo "=== top ==="
echo ""
echo "   Number of wires:                 12"
echo "   Number of wire bits:             70"
echo "   Number of public wires:           6"
echo "   Number of public wire bits:      14"
echo "   Number of memories:               0"
echo "   Number of memory bits:            0"
echo "   Number of processes:              0"
echo "   Number of cells:                 29"
echo "     DFFR                           24"
echo "     LUT2                            1"
echo "     LUT4                            3"
echo "     IOBUF                           9"
echo ""
echo "Warning: Identifier \`counter' is implicitly declared in module \`top'."

top=$(sed -n 's/.*-top[[:space:]]\+\([A-Za-z_][A-Za-z0-9_$]*\).*/\1/p' "$script" | head -1)
[ -n "$top" ] || top=top
out=$(sed -n 's/^write_json "\(.*\)"$/\1/p' "$script" | head -1)

if [ -z "$out" ]; then
  echo "ERROR: stub yosys: no write_json target found in script: $script" >&2
  exit 2
fi

mkdir -p "$(dirname "$out")"
cat > "$out" <<JSON
{
  "creator": "Yosys 0.38+stub",
  "modules": {
    "$top": {
      "attributes": { "top": "00000000000000000000000000000001" },
      "parameter_default_values": {},
      "ports": {
        "clk":   { "direction": "input",  "bits": [ 2 ] },
        "rst_n": { "direction": "input",  "bits": [ 3 ] },
        "btn":   { "direction": "input",  "bits": [ 4 ] },
        "led":   { "direction": "output", "bits": [ 5 ] },
        "leds":  { "direction": "output", "bits": [ 6, 7, 8, 9, 10, 11 ] }
      },
      "cells": {
        "counter.0": { "hide_name": 1, "type": "DFFR", "parameters": {},
                       "connections": { "C": [ 2 ], "D": [ 12 ], "Q": [ 12 ], "R": [ 3 ] } }
      },
      "netnames": {
        "counter": { "hide_name": 0, "bits": [ 12, 13 ], "attributes": {} },
        "led":     { "hide_name": 0, "bits": [ 5 ], "attributes": {} }
      }
    }
  }
}
JSON
echo ""
echo "End of script. Logfile hash: stub"
exit 0
)SH";

    // 桩 nextpnr：接受 --json/--write/--vopt/--device，产出 .pnr.json。
    const std::string nextpnrStub = R"SH(#!/usr/bin/env bash
set -u
json=""; write=""; cst=""
while [ $# -gt 0 ]; do
  case "$1" in
    --json) json="$2"; shift 2;;
    --write) write="$2"; shift 2;;
    --vopt) case "$2" in cst=*) cst="${2#cst=}";; esac; shift 2;;
    --device|--himbaechel-device) shift 2;;
    *) shift;;
  esac
done

echo "Info: nextpnr-himbaechel -- Next Place and Route -- for Gowin GW1N-9C"
echo "Info: Version 0.7 (git sha1 stub0000, built for Linux probe)"
echo "Info: Using uarch 'GW1N-9C' for device 'GW1NR-LV9QN88PC6/I5'"
echo "Info: Using JSON netlist: $json"
[ -n "$cst" ] && echo "Info: Using constr file: $cst"
echo "Info: Loading JSON netlist"
echo "Info: Annotating ports with timing estimates."
# 下面是 nextpnr-himbaechel 的**真实措辞**（"Packing"/"Placing"/"Routing"），
# 刻意不用 "Pack IOBs" 这种旧解析器才认的写法，用于验证大小写/词干匹配。
echo "Info: Packing constants.."
echo "Info: Packing IOs.."
echo "Info: Packing IOBs.."
echo "Info: Packing GSR.."
echo "Info: Packing wide LUTs.."
echo "Info: Creating initial analytic placement for 24 cells..."
echo "Info: Running main analytical placer."
echo "Info: HeAP Placer Time: 0.01s"
echo "Info: Placing cells."
echo "Info: Routing.."
echo "Info: Setting up routing queue."
echo "Info: Routing globals..."
echo "Info: Routing complete."
echo "Info: Device utilisation:"
echo "Info:      DFF:     24/ 8640    0%"
echo "Info:     LUT4:      4/ 8640    0%"
echo "Info:    IOBUF:      9/  274    3%"
echo "Info: Max frequency for clock 'clk': 128.53 MHz (PASS at 27.00 MHz)"
echo "Info: Program finished. Writing design to $write"

if [ -z "$write" ]; then
  echo "ERROR: stub nextpnr: no --write target" >&2
  exit 2
fi
mkdir -p "$(dirname "$write")"
cat > "$write" <<JSON
{
  "module": "top",
  "device": "GW1NR-LV9QN88PC6/I5",
  "fmax": { "clk": 128.53 },
  "utilisation": { "DFF": 24, "LUT4": 4, "IOBUF": 9 }
}
JSON
exit 0
)SH";

    if (!WriteFileUtf8(yosysPath, yosysStub)) return false;
    if (!WriteFileUtf8(nextpnrPath, nextpnrStub)) return false;

    // 加可执行权限（POSIX）。Windows 上不需要。
#if !defined(_WIN32)
    const std::string chmodCmd =
        "chmod +x '" + std::string(yosysPath.ToUTF8().data()) + "' '" +
        std::string(nextpnrPath.ToUTF8().data()) + "'";
    if (std::system(chmodCmd.c_str()) != 0) {
        std::printf("[probe] warning: chmod failed for stub tools\n");
    }
#endif
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    wxInitializer initializer;
    if (!initializer.IsOk()) {
        std::fprintf(stderr, "wx initialisation failed\n");
        return 2;
    }
    wxLog::SetActiveTarget(new wxLogStderr());

    const wxString workDir = argc > 1
        ? wxString::FromUTF8(argv[1])
        : sigflow::platform::JoinPath(wxGetCwd(), "fpga_probe_work");
    const wxString toolDirArg = argc > 2 ? wxString::FromUTF8(argv[2]) : wxString();
    const wxString step = argc > 3 ? wxString::FromUTF8(argv[3]) : "all";

    const bool useStubs = toolDirArg.IsEmpty();
    const wxString toolDir = useStubs
        ? sigflow::platform::JoinPath(workDir, "tools") : toolDirArg;

    wxString yosysPath;
    wxString nextpnrPath;
    if (useStubs) {
        if (!WriteStubTools(toolDir, yosysPath, nextpnrPath)) {
            std::fprintf(stderr, "unable to create stub tools\n");
            return 2;
        }
    } else {
        yosysPath = sigflow::platform::JoinPath(toolDir,
            sigflow::platform::WithExecutableSuffix("yosys"));
        nextpnrPath = sigflow::platform::JoinPath(toolDir,
            sigflow::platform::WithExecutableSuffix("nextpnr-himbaechel"));
    }

    const wxString projectDir = sigflow::platform::JoinPath(workDir, "proj");
    const wxString srcDir = sigflow::platform::JoinPath(projectDir, "src");
    const wxString cstDir = sigflow::platform::JoinPath(projectDir, "constraints");
    const wxString jobDir = sigflow::platform::JoinPath(workDir, "job");
    const wxString topModule = "top";
    const wxString sourcePath = sigflow::platform::JoinPath(srcDir, "top.v");
    const wxString cstPath = sigflow::platform::JoinPath(cstDir, "top.cst");
    const wxString yosysScriptPath = sigflow::platform::JoinPath(jobDir, "run_yosys.ys");
    const wxString jsonPath = sigflow::platform::JoinPath(jobDir, "top.json");
    const wxString pnrPath = sigflow::platform::JoinPath(jobDir, "top.pnr.json");

    std::printf("workDir     = %s\n", workDir.ToUTF8().data());
    std::printf("toolDir     = %s%s\n", toolDir.ToUTF8().data(),
                useStubs ? "  (桩工具)" : "");
    std::printf("yosys       = %s\n", yosysPath.ToUTF8().data());
    std::printf("nextpnr     = %s\n", nextpnrPath.ToUTF8().data());
    std::printf("step        = %s\n", step.ToUTF8().data());

    // ---------------------------------------------------------------- prep
    if (step == "all" || step == "prep") {
        Section("STEP 1: 准备测试工程");
        wxFileName::Mkdir(srcDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        wxFileName::Mkdir(cstDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        wxFileName::Mkdir(jobDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

        const std::string projectJson =
            "{\n"
            "  \"build\": { \"top_module\": [\"top\"] },\n"
            "  \"paths\": { \"source_files\": [\"src/top.v\"], \"library_files\": [] },\n"
            "  \"fpga\": {\n"
            "    \"target_profile\": \"tang-nano-9k\",\n"
            "    \"yosys_path\": \"\",\n"
            "    \"yosys_strategy\": \"baseline\",\n"
            "    \"nextpnr_path\": \"\",\n"
            "    \"nextpnr_args\": [],\n"
            "    \"openfpgaloader_path\": \"\",\n"
            "    \"openfpgaloader_args\": []\n"
            "  }\n"
            "}\n";
        WriteFileUtf8(sigflow::platform::JoinPath(projectDir, "sigflow.project"), projectJson);

        // 设计文件从仓库里的 tests/fpga_flow/ 复制过来（找不到就报错，不做隐形兜底）。
        const wxString designSource =
            sigflow::platform::JoinPath(sigflow::platform::ExecutableDir(),
                                        "fpga_flow/top.v");
        const wxString cstSource =
            sigflow::platform::JoinPath(sigflow::platform::ExecutableDir(),
                                        "fpga_flow/top.cst");
        std::string design = ReadFileUtf8(designSource);
        std::string cst = ReadFileUtf8(cstSource);
        if (design.empty() || cst.empty()) {
            std::printf("[probe] FATAL: 找不到设计/约束文件:\n  %s\n  %s\n",
                        designSource.ToUTF8().data(), cstSource.ToUTF8().data());
            return 3;
        }
        WriteFileUtf8(sourcePath, design);
        WriteFileUtf8(cstPath, cst);

        Note("工程目录: " + projectDir);
        Note("顶层文件: " + sourcePath + "  (" + wxString::Format("%lu", static_cast<unsigned long>(design.size())) + " 字节)");
        Note("约束文件: " + cstPath + "  (" + wxString::Format("%lu", static_cast<unsigned long>(cst.size())) + " 字节)");
        Note("sigflow.project 已写入");
    }

    FpgaTargetProfile profile;
    wxString profileError;
    if (!ResolveFpgaTargetProfile("tang-nano-9k", profile, profileError)) {
        std::printf("[probe] FATAL: %s\n", profileError.ToUTF8().data());
        return 3;
    }
    std::printf("\nprofile: id=%s device=%s family=%s yosysFamily=%s\n",
                profile.id.ToUTF8().data(), profile.device.ToUTF8().data(),
                profile.family.ToUTF8().data(), profile.yosysFamily.ToUTF8().data());

    // ---------------------------------------------------------------- preflight
    if (step == "all" || step == "preflight") {
        Section("STEP 2: Yosys Runtime 预检 (ValidateYosysRuntime)");
        const FpgaYosysRuntimeReport report = ValidateYosysRuntime(yosysPath);
        Note(report.FormatForTerminal());
        std::printf(">>> valid=%d  shareDirectory=%s\n", report.valid ? 1 : 0,
                    report.shareDirectory.ToUTF8().data());
        int failedRequired = 0;
        for (const auto& check : report.checks) {
            std::printf("    %-28s required=%-5s passed=%-5s %s\n",
                        check.id.ToUTF8().data(),
                        check.required ? "true" : "false",
                        check.passed ? "true" : "false",
                        check.path.ToUTF8().data());
            if (check.required && !check.passed) ++failedRequired;
        }
        std::printf(">>> required 检查失败数 = %d\n", failedRequired);

        // 约束文件校验（4 步管线：存在 → 非空 → 语法 → 端口覆盖）
        const std::vector<wxString> topPorts = {
            "clk", "rst_n", "btn", "led",
            "leds[0]", "leds[1]", "leds[2]", "leds[3]", "leds[4]", "leds[5]"
        };
        const CstValidationResult cstResult = CstValidator::Validate(cstPath, topPorts);
        std::printf(">>> CST valid=%d exists=%d nonEmpty=%d syntaxOk=%d "
                    "行=%d IO_LOC=%d IO_PORT=%d 注释=%d 错误行=%d 已绑定端口=%zu\n",
                    cstResult.valid ? 1 : 0, cstResult.fileExists ? 1 : 0,
                    cstResult.fileNonEmpty ? 1 : 0, cstResult.syntaxOk ? 1 : 0,
                    cstResult.totalLines, cstResult.ioLocCount, cstResult.ioPortCount,
                    cstResult.commentLines, cstResult.invalidLines,
                    cstResult.boundPorts.size());
        for (const auto& lineError : cstResult.lineErrors) {
            std::printf("    CST line %d: %s | %s\n", lineError.line,
                        lineError.content.ToUTF8().data(),
                        lineError.reason.ToUTF8().data());
        }
        if (!cstResult.errorSummary.IsEmpty()) {
            std::printf("    CST summary: %s\n", cstResult.errorSummary.ToUTF8().data());
        }
    }

    // ---------------------------------------------------------------- script
    if (step == "all" || step == "script") {
        Section("STEP 3: 生成 Yosys 脚本 (FpgaYosysScriptGenerator)");
        FpgaYosysScriptRequest request;
        request.sourceFiles = { sourcePath };
        request.topModule = topModule;
        request.targetProfile = profile;
        request.strategy = FpgaYosysSynthesisStrategy::Baseline;
        request.outputJsonPath = jsonPath;

        FpgaYosysScriptGenerator generator;
        const FpgaYosysScriptResult result = generator.Generate(request);
        if (!result.success) {
            std::printf(">>> 生成失败: %s\n", result.errorMessage.ToUTF8().data());
        } else {
            WriteFileUtf8(yosysScriptPath, std::string(result.script.ToUTF8().data()));
            Note("--- run_yosys.ys ---");
            Note(result.script);
            Note("--- end ---");
            std::printf(">>> 脚本已写入 %s\n", yosysScriptPath.ToUTF8().data());
        }
    }

    // ---------------------------------------------------------------- synth
    if (step == "all" || step == "synth") {
        Section("STEP 4: 运行 Yosys (YosysExecutor + PlatformProcess)");
        YosysExecutor executor;
        YosysExecutor::Config config;
        config.workingDirectory = jobDir;
        config.timeLimitSec = 60;
        config.combinedLogPath = sigflow::platform::JoinPath(jobDir, "yosys.combined.log");

        const std::vector<wxString> args = { "-q", "-s", yosysScriptPath };
        std::printf(">>> exec: %s -q -s %s\n", yosysPath.ToUTF8().data(),
                    yosysScriptPath.ToUTF8().data());

        bool started = false;
        const YosysExecutor::Result result =
            RunSync(executor, yosysPath, args, config, started);

        std::printf(">>> started=%d exitCode=%d combinedLogWritten=%d\n",
                    started ? 1 : 0, result.exitCode, result.combinedLogWritten ? 1 : 0);
        Note("--- combined log ---");
        Note(result.combinedLog);
        Note("--- end ---");

        Section("STEP 5: 校验 Yosys JSON 产物 (ArtifactValidator)");
        ArtifactValidator validator;
        NetlistArtifactReport artifact;
        const bool valid = validator.ValidateYosysJson(jsonPath, topModule, artifact);
        std::printf(">>> valid=%d status=%d size=%llu ports=%llu cells=%llu nets=%llu\n",
                    valid ? 1 : 0, static_cast<int>(artifact.status),
                    static_cast<unsigned long long>(artifact.sizeBytes),
                    static_cast<unsigned long long>(artifact.portCount),
                    static_cast<unsigned long long>(artifact.cellCount),
                    static_cast<unsigned long long>(artifact.netnameCount));
        std::printf(">>> message: %s\n", artifact.message.ToUTF8().data());

        Section("STEP 6: 解析 Yosys 日志 (FpgaYosysLogParser)");
        FpgaYosysLogParser parser;
        const YosysLogRecord record = parser.Parse(result.combinedLog);
        std::printf(">>> toolVersion=%s status=%s warnings=%d errors=%d failedStage=%s\n",
                    record.toolVersion.ToUTF8().data(), record.status.ToUTF8().data(),
                    record.warningCount, record.errorCount, record.failedStage.ToUTF8().data());
        std::printf(">>> rootCause=%s\n", record.rootCause.ToUTF8().data());
        std::printf(">>> totalCells=%d lut=%d dff=%d ibuf=%d obuf=%d stages=%zu events=%zu\n",
                    record.resources.totalCells, record.resources.lutCount,
                    record.resources.dffCount, record.resources.ibufCount,
                    record.resources.obufCount, record.stages.size(),
                    record.events.size());
        for (const auto& item : record.resources.cellTypes) {
            std::printf("    cell %-10s %d\n", item.type.ToUTF8().data(), item.count);
        }
        for (const auto& stage : record.stages) {
            std::printf("    stage %-22s lines %d-%d\n", stage.name.ToUTF8().data(),
                        stage.firstLine, stage.lastLine);
        }
        for (const auto& event : record.events) {
            std::printf("    event sev=%s rule=%s line=%d src=%s:%d raw=%s\n",
                        ToString(event.severity).ToUTF8().data(),
                        event.ruleId.ToUTF8().data(), event.lineNumber,
                        event.sourceFile.ToUTF8().data(), event.sourceLine,
                        event.rawLine.ToUTF8().data());
        }
    }

    // ---------------------------------------------------------------- pnr
    if (step == "all" || step == "pnr") {
        Section("STEP 7: nextpnr 布局布线 (NextpnrExecutor::Prepare)");
        NextpnrExecutor executor;
        NextpnrExecuteRequest request;
        request.projectPath = projectDir;
        request.topModule = topModule;
        request.jsonPath = jsonPath;
        request.configuredCstPath = cstPath;
        request.deviceName = profile.device;
        request.familyName = profile.family;
        request.executablePath = nextpnrPath;
        request.outputDirectory = jobDir;

        wxString prepareError;
        if (!executor.Prepare(request, prepareError)) {
            std::printf(">>> Prepare 失败: %s\n", prepareError.ToUTF8().data());
        } else {
            const std::vector<wxString>& args = executor.GetArguments();
            std::string joined;
            for (const wxString& arg : args) {
                joined += " '";
                joined += arg.ToUTF8().data();
                joined += "'";
            }
            std::printf(">>> argv (%zu):%s\n", args.size(), joined.c_str());

            NextpnrExecutor::Config config;
            config.workingDirectory = jobDir;
            config.timeLimitSec = 60;
            config.combinedLogPath = sigflow::platform::JoinPath(jobDir, "nextpnr.combined.log");

            bool started = false;
            const NextpnrExecutor::Result result =
                RunSync(executor, nextpnrPath, args, config, started);
            std::printf(">>> started=%d exitCode=%d\n", started ? 1 : 0, result.exitCode);
            Note("--- nextpnr combined log ---");
            Note(result.combinedLog);
            Note("--- end ---");

            Section("STEP 8: nextpnr 结果分析 (Finalize + ValidateArtifact)");
            const NextpnrJobResult jobResult = executor.Finalize(result.exitCode, result.combinedLog);
            std::printf(">>> succeeded=%d exitCode=%d elapsed=%.2fs\n",
                        jobResult.succeeded ? 1 : 0, jobResult.exitCode, jobResult.elapsedSec);
            std::printf(">>> pnrJson=%s\n", jobResult.pnrJsonPath.ToUTF8().data());
            std::printf(">>> analysisJson=%s\n", jobResult.analysisJsonPath.ToUTF8().data());
            Note(">>> terminalSummary: " + jobResult.terminalSummary);
            std::printf(">>> 产物存在: pnr=%d (size=%s)\n",
                        wxFileExists(pnrPath) ? 1 : 0,
                        wxFileName(pnrPath).GetSize() == wxInvalidSize
                            ? "n/a"
                            : wxString::Format("%llu",
                                  static_cast<unsigned long long>(
                                      wxFileName(pnrPath).GetSize().GetValue())).ToUTF8().data());
        }
    }

    std::printf("\nfpga_flow_probe done.\n");
    return 0;
}
