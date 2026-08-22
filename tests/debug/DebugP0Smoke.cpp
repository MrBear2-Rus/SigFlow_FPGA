// P0 设计冻结 smoke：调试契约、指纹、会话状态机、目录与清理。
#include "../../main/debug/DebugContract.h"
#include "../../main/debug/DebugFingerprint.h"
#include "../../main/debug/DebugSession.h"
#include "../../main/debug/DebugThresholds.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace sigflow::debug;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& message)
{
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL: " << message << "\n";
    }
}

std::string ValidContractJson(const std::string& sessionId)
{
    return R"({
  "schema_version": "1.0",
  "session_id": ")" + sessionId + R"(",
  "target_profile": "tang-nano-9k@1.0.0",
  "top_module": "uart_top",
  "sample_clock": { "signal": "clk_27m", "frequency_hz": 27000000 },
  "probes": [
    { "id": "tx_busy", "path": "uart_top.tx_busy", "width": 1 },
    { "id": "state", "path": "uart_top.state", "width": 4 }
  ],
  "capture": { "depth": 1024, "pretrigger_samples": 512 },
  "transport": { "tx_port": "UART_TX", "rx_port": "UART_RX" }
})";
}

void TestContract()
{
    DebugContract contract;
    std::string error;
    Check(contract.ParseJson(ValidContractJson("T-P0-000001"), error),
          "contract parse: " + error);
    contract.ApplyDefaults();
    Check(contract.capture.depth == DebugDefaults::kCaptureDepth, "default depth");
    Check(contract.capture.pretriggerSamples == DebugDefaults::kPretriggerSamples,
          "default pretrigger");
    Check(contract.transport.baud == DebugDefaults::kUartBaud, "default baud 921600");
    Check(contract.probes.size() == 2, "probe count");
    Check(contract.AssignProbeBitOffsets(error), "assign bit offsets: " + error);
    Check(contract.probes[1].bitOffset == 1, "second probe bit offset 1");
    Check(contract.Validate(error), "contract validate: " + error);

    // 非法：总宽度超 32
    DebugContract wide;
    Check(wide.ParseJson(ValidContractJson("T-P0-000003"), error), "parse wide");
    wide.probes[0].width = 33;
    Check(!wide.Validate(error), "reject width > 32");

    // 非法：pretrigger > depth
    DebugContract badPre;
    Check(badPre.ParseJson(ValidContractJson("T-P0-000004"), error), "parse bad pre");
    badPre.capture.pretriggerSamples = badPre.capture.depth + 1;
    Check(!badPre.Validate(error), "reject pretrigger > depth");

    // 非法：未知波特率
    DebugContract badBaud;
    Check(badBaud.ParseJson(ValidContractJson("T-P0-000005"), error), "parse bad baud");
    badBaud.transport.baud = 123456;
    Check(!badBaud.Validate(error), "reject unknown baud");

    // 非法：空 top_module
    DebugContract badTop;
    Check(badTop.ParseJson(ValidContractJson("T-P0-000006"), error), "parse bad top");
    badTop.topModule.clear();
    Check(!badTop.Validate(error), "reject empty top");

    // 序列化往返
    const std::string serialized = contract.ToJson();
    DebugContract roundTrip;
    Check(roundTrip.ParseJson(serialized, error), "round trip parse: " + error);
    Check(roundTrip.probes.size() == contract.probes.size() &&
              roundTrip.transport.baud == contract.transport.baud &&
              roundTrip.capture.depth == contract.capture.depth,
          "round trip fields");
}

void TestFingerprint()
{
    const std::string dir =
        (std::filesystem::temp_directory_path() / "sigflow_p0_fp").string();
    std::filesystem::create_directories(dir);
    const std::string fileA = dir + "\\a.v";
    const std::string fileB = dir + "\\b.v";
    {
        std::ofstream out(fileA, std::ios::trunc);
        out << "module top;\nendmodule\n";
    }
    {
        std::ofstream out(fileB, std::ios::trunc);
        out << "module top;\n  wire x;\nendmodule\n";
    }

    DebugFingerprintInputs inputs;
    inputs.sourceFiles = { fileA };
    inputs.topModule = "top";
    inputs.constraintsContent = "IO_LOC \"clk\" 52;";
    inputs.targetProfile = "tang-nano-9k@1.0.0";
    inputs.probeSignature = "tx_busy:1:0,state:4:1";

    const std::string fp1 = ComputeSourceFingerprint(inputs);
    const std::string fp2 = ComputeSourceFingerprint(inputs);
    Check(fp1.size() == 64 && fp1 == fp2, "fingerprint stable");

    inputs.probeSignature = "tx_busy:1:0,state:4:2";
    const std::string fp3 = ComputeSourceFingerprint(inputs);
    Check(fp3 != fp1, "fingerprint sensitive to probe signature");

    inputs.sourceFiles = { fileA, fileB };
    const std::string fp4 = ComputeSourceFingerprint(inputs);
    Check(fp4 != fp1, "fingerprint sensitive to file set");

    const std::string toolchain1 =
        ComputeToolchainFingerprint({ "yosys 0.38", "nextpnr 0.5" }, "sf_micro_ila@1.0");
    const std::string toolchain2 =
        ComputeToolchainFingerprint({ "yosys 0.38", "nextpnr 0.6" }, "sf_micro_ila@1.0");
    Check(toolchain1.size() == 64 && toolchain1 != toolchain2,
          "toolchain fingerprint sensitive");

    const std::uint64_t fp64 = Fingerprint64FromHex(fp1);
    Check(fp64 != 0, "fingerprint64 non-zero");
    Check(Fingerprint64FromHex(fp1) == fp64, "fingerprint64 stable");

    const std::string fileHash1 = Sha256File(fileA);
    const std::string fileHash2 = Sha256File(fileA);
    Check(fileHash1.size() == 64 && fileHash1 == fileHash2, "file sha256 stable");
    Check(Sha256String("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "known sha256 vector");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void TestStateMachine()
{
    Check(IsLegalDebugTransition(DebugSessionState::Created,
                                 DebugSessionState::Validating),
          "created->validating");
    Check(IsLegalDebugTransition(DebugSessionState::Validating,
                                 DebugSessionState::Building),
          "validating->building");
    Check(IsLegalDebugTransition(DebugSessionState::Building,
                                 DebugSessionState::Programming),
          "building->programming");
    Check(IsLegalDebugTransition(DebugSessionState::Programming,
                                 DebugSessionState::Armed),
          "programming->armed");
    Check(IsLegalDebugTransition(DebugSessionState::Armed,
                                 DebugSessionState::Captured),
          "armed->captured");
    Check(IsLegalDebugTransition(DebugSessionState::Captured,
                                 DebugSessionState::Compared),
          "captured->compared");
    Check(IsLegalDebugTransition(DebugSessionState::Armed,
                                 DebugSessionState::TimedOut),
          "armed->timedout");
    Check(IsLegalDebugTransition(DebugSessionState::Armed,
                                 DebugSessionState::Cancelled),
          "armed->cancelled");
    Check(IsLegalDebugTransition(DebugSessionState::Compared,
                                 DebugSessionState::Armed),
          "compared->armed (re-arm)");

    Check(!IsLegalDebugTransition(DebugSessionState::Created,
                                  DebugSessionState::Compared),
          "reject created->compared");
    Check(!IsLegalDebugTransition(DebugSessionState::Created,
                                  DebugSessionState::Armed),
          "reject created->armed");
    Check(!IsLegalDebugTransition(DebugSessionState::Failed,
                                  DebugSessionState::Armed),
          "reject failed->armed");
    Check(!IsLegalDebugTransition(DebugSessionState::Compared,
                                  DebugSessionState::Validating),
          "reject compared->validating");
    Check(IsTerminalDebugSessionState(DebugSessionState::Compared) &&
              IsTerminalDebugSessionState(DebugSessionState::Failed) &&
              IsTerminalDebugSessionState(DebugSessionState::TimedOut) &&
              IsTerminalDebugSessionState(DebugSessionState::Cancelled),
          "terminal states");
}

void TestSessionService()
{
    const std::string project =
        (std::filesystem::temp_directory_path() / "sigflow_p0_project").string();
    std::filesystem::create_directories(project + "\\.sigflow\\sim");
    const std::string simMarker = project + "\\.sigflow\\sim\\keep.txt";
    {
        std::ofstream out(simMarker, std::ios::trunc);
        out << "keep\n";
    }

    DebugSessionService service;
    DebugContract contract;
    std::string error;
    Check(contract.ParseJson(ValidContractJson("T-P0-SESSION-1"), error),
          "service contract: " + error);
    contract.ApplyDefaults();

    DebugSessionInfo session;
    Check(service.Create(project, contract, session, error),
          "session create: " + error);
    const DebugSessionPaths paths = DebugSessionService::GetPaths(project, session.id);
    Check(std::filesystem::exists(paths.manifest) &&
              std::filesystem::exists(paths.overlay) &&
              std::filesystem::exists(paths.logs),
          "session dirs created");

    // manifest 往返
    DebugSessionInfo loaded;
    Check(service.Load(project, session.id, loaded, error),
          "session load: " + error);
    Check(loaded.state == DebugSessionState::Created &&
              loaded.transitions.size() == 1,
          "loaded fields");

    // 状态链
    Check(service.Transition(project, session.id, DebugSessionState::Validating,
                             "validating", 0, error),
          "transition validating: " + error);
    Check(service.Transition(project, session.id, DebugSessionState::Building,
                             "building", 0, error),
          "transition building");
    Check(service.Transition(project, session.id, DebugSessionState::Programming,
                             "programming", 0, error),
          "transition programming");
    Check(service.Transition(project, session.id, DebugSessionState::Armed,
                             "armed", 0, error),
          "transition armed");
    Check(service.Transition(project, session.id, DebugSessionState::TimedOut,
                             "timeout", -1, error),
          "transition timedout: " + error);
    Check(service.Load(project, session.id, loaded, error) &&
              loaded.state == DebugSessionState::TimedOut,
          "state persisted as TimedOut");

    // 非法迁移被拒绝
    Check(!service.Transition(project, session.id, DebugSessionState::Armed,
                              "illegal", 0, error),
          "reject illegal transition from terminal");

    // 取消
    DebugContract cancelContract;
    Check(cancelContract.ParseJson(ValidContractJson("T-P0-SESSION-2"), error),
          "cancel contract");
    cancelContract.ApplyDefaults();
    DebugSessionInfo cancelSession;
    Check(service.Create(project, cancelContract, cancelSession, error),
          "cancel create");
    Check(service.Cancel(project, cancelSession.id, "user cancel", error),
          "cancel session: " + error);

    // 清理策略：保留 1 个，删掉更旧的终态会话，不碰 sim/
    std::vector<std::string> removed;
    Check(service.CleanupOldSessions(project, 1, removed, error) >= 1,
          "cleanup removed old sessions: " + error);
    Check(std::filesystem::exists(simMarker), "sim dir untouched");

    std::error_code ec;
    std::filesystem::remove_all(project, ec);
}

} // namespace

int main()
{
    TestContract();
    TestFingerprint();
    TestStateMachine();
    TestSessionService();

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cout << g_failures << " FAILURES\n";
    return 1;
}
