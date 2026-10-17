# 阶段 16：真实 Verilator 执行链与教学反馈

日期：2026-10-16。范围对应 UA-05 多步执行与 UA-07 的真实仿真依赖，实施步骤见[阶段计划](u2-stage16-simulation-plan.md)。Python 服务和可选集成宿主保留在 sigflowAgent；本次另补齐官方 Verilator 插件的产物摘要，不迁移旧 agent。

## 执行范围

运行器在自己创建的临时工程中先执行真实 Yosys 基线，然后通过常驻教学宿主发布 build/run 计划。Gateway UI 独立身份签发 max_jobs=2 的精确步骤 grant；教学宿主仅持 Gateway Agent 身份。Verilator build 的可执行产物由 Gateway 按同一快照和前序 Job 解析，run 不接受 Agent 指定的本机可执行路径。

长 build 超过宿主同步等待期限时进入 RecoveryRequired。运行器通过 Gateway GET 等待已知 Job 完成，再调用 observe_verification。成功观察 build 必须返回 WaitingApproval，且工程仍只有基线和 build 两个 Job；随后显式 execute_plan 才能启动 run。验收脚本直接校验该转换，即使错误实现自动执行 run 并返回 Completed，也会拒绝通过。完整原审批在短任务的同步路径中仍可顺序执行两步。

最终核对两个新 Core 报告的项目/revision/snapshot/Job、capability/plugin、完整性和退出码，以及新的 evidence IDs。run 的 VCD 还通过认证 metadata 校验 artifact ID、Job、revision、schema、SHA-256 和非零大小。元数据核验不等于读取或验证精确波形采样。

## 当前 harness 的限制

现有官方插件只生成一次 eval/dump 的 harness，不驱动时钟，也不推进时间。验收明确记录 harness=single_evaluation、time_advance_verified=false、functional_correctness=not_evaluated；反馈维持 outcome_indeterminate。真实 build/run 通过仅证明当前工具执行链和产物可追溯，不能证明动态测试、复位行为、学生预测或硬件功能正确。

`context.waveform.check_waveform(origin='tool')` 的完整性门禁保持原样。S2 末信号因事件/字节限额被截断后可能仍标 exact，详见[阶段 11](stage-11-waveform.md)。本阶段没有将真实数据改标 fixture，也没有解锁八类 detector 或 L4。

## 环境与兼容修正

Windows 集成场景使用仓库已有的 Verilator、MinGW/MSVCRT 编译器与 make；Git 安装提供 sh/Perl，Python 使用当前解释器的实际基础可执行文件。无需激活 Conda 或安装 UCAgent。日常 Python 服务仍只依赖标准库；这些原生工具仅用于显式真实仿真验收。其他机器必须准备对应工具，缺失直接失败 simulation_tools_missing，不按跳过处理。

`simulation_environment` 为本次 Core 子进程构造环境，将仓库 mingw32-make.exe 复制为自己临时目录下的 make.exe。Git shell 目录根据当前 Git 路径解析，不硬编码本机安装位置。不会修改用户全局 PATH、第三方安装文件或 shell 配置。

本机 verilated.mk 写死了打包机器上的 MSVC 和 Python 路径；实际构建曾因此返回退出码 1。通过私有 MAKEFLAGS 的命令变量 CXX=g++、LINK=g++、AR=ar、PYTHON3=python3 覆盖这些配置，并将 VERILATOR_ROOT 规范化为正斜杠。真实 make 回归验证文件中的旧绝对路径不会覆盖这些选择，继承的 MFLAGS/GNUMAKEFLAGS 不影响本次选择。

另一处真实阻塞为 Verilator build/run 注册产物时未填写 SHA-256：工具虽成功，报告 sha256=null，U2 仍拒绝取证。两个官方插件现于产物完成后计算 SHA-256；摘要计算失败不发布成功产物。原有声明的 report/metadata 归属检查不放宽；Gateway metadata 按摘要重算文件，后续篡改不能沿用该事实。

## 构建与运行

在仓库根目录执行，复用已有构建依赖；不下载新工具：

```powershell
cmake -S . -B build-main-msvcrt -DSIGFLOW_BUILD_TESTS=OFF -DSIGFLOW_BUILD_U2_INTEGRATION=ON
cmake --build build-main-msvcrt --target edu_u2_gateway edu_u2_sim_artifact_test -j 4
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
& sigflowAgent/.venv/Scripts/python.exe -P -m sigflow_edu_agent.host.closed_loop --gateway build-main-msvcrt/sigflowAgent/integration/edu_u2_gateway.exe --scenario simulation --mode rule --timeout 300 --output sigflowAgent/docs/closed-loop-stage16-simulation.json
```

必须等待构建退出成功再运行，避免使用上一版本宿主。simulation 场景本阶段只支持 rule，不与 --courses 或 DeepSeek 组合；参数组合在启动子进程前拒绝。其他 same-revision、student-edit 和 --courses 场景保持原入口。

Core bootstrap 的可选 simulation 布尔字段仅经运行器持有的 stdin 传入；缺省 false，无仿真工具探测或仿真 Provider 注册。教学 HTTP 不接受这一字段或任何工具路径。

## 生产方摘要回归

edu_u2_sim_artifact_test 是显式构建的独立 C++ 测试目标。它直接调用真实插件和 CoreJobContext，以 Fake IProcessHost 代替外部执行，生成固定内容的可执行/VCD 测试文件，并对独立计算的固定 SHA-256 字面值断言。不运行真实 Verilator，不代表真实工具验收，不删除任何文件。

下述 Python 包装只清理自己刚创建的临时目录：

```powershell
$env:PYTHONPATH=(Resolve-Path sigflowAgent).Path
@'
from pathlib import Path
import subprocess, tempfile
from sigflow_edu_agent.host.simulation_acceptance import simulation_environment
with tempfile.TemporaryDirectory(prefix='sigflow-u2-artifact-') as owned:
    root = Path(owned)
    (root/'test-owned').touch()
    env = simulation_environment(root)
    result = subprocess.run([str(Path('build-main-msvcrt/sigflowAgent/integration/edu_u2_sim_artifact_test.exe').resolve()), owned], env=env, timeout=15)
    raise SystemExit(result.returncode)
'@ | & sigflowAgent/.venv/Scripts/python.exe -P -
```

修复前实测 exit=1、build_hash=0、run_hash=0；修复后 exit=0、两项均为 1。测试文件和临时工程只由持有的运行器管理，不按固定目录名删除已有工程或进程。

## 测试与再规划

定向测试覆盖缺工具、私有环境、真实 make 覆盖、报告/产物绑定篡改、错误参数组合以及禁止观察时自动执行后继。独立审查发现的后继验收缺口已形成先失败后通过的回归。

阶段机器报告：[真实执行](closed-loop-stage16-simulation.json)、[开发评测](evaluation-stage16.json)、[验收汇总](verification-stage16.json)。最终运行数字和摘要由验收汇总记录；课程数据库和 U3 检索算法未修改。

最终全量 unittest 运行 514 项，无失败，耗时 72.904 秒；逐项成功 481、单项跳过 33，另有 2 个类级跳过覆盖 18 个未运行方法。共收集 532 个方法，35 条跳过记录不能直接从 testsRun 扣除。单项跳过为 6 项历史 UCAgent 实验和 27 项缺课程原始快照；类级跳过也因原始快照缺失。正常 Windows 当前用户上下文的 DPAPI 测试只使用虚构 Key。

开发评测 40/40（37 规则、3 Mock），仅为合成开发场景，源码摘要与最终代码匹配。真实场景完成 3 Job、grant 使用量 [1,2]、1 次只读观察、1 次后继确认；新旧证据不同，旧版本和错误 build 依赖拒绝，动作重放不增加 Job，394 字节 VCD 的 hash/schema/归属核验通过。关闭仿真的[课程兼容场景](closed-loop-stage16-courses.json)另完成 2 个真实 Yosys Job、4 条背景元数据核验，partial/not_assessed，规则卡实际引用 0。两场景均模型调用 0、清理完成。独立审查修复复核 PASS；真实精确波形及动态采样未验收。

下一阶段先修复并测试 S2 波形截断语义，再实现可信单页查询和显式学生预测动作；动态 stimuli、GUI 保存同步、真实 DeepSeek 课程质量与双人教师评审分别签收。本阶段不提交、不推送。
