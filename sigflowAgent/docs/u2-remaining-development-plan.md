# U2 现有依赖下的剩余开发计划

日期：2026-10-16。工作基线 8b067b2，接续阶段 16。执行授权为继续完成现有依赖下可完成的全部 U2 工作；按阶段实现、测试、审查并更新本文件，不进行暂存、提交或推送。

## 依赖结论

U2 依赖未全部完成。事务宿主、受控执行、真实 Yosys、Verilator 单次求值、课程接口已有运行证据；当前可直接补齐波形服务完整性、可信读取、学生预测与已存在 receipt 接口的 U2 门控。动态激励需要受控输入契约；教师审阅和评分、真实 GUI 签收、原生 Linux/麒麟环境及团队冻结集不能由本机代码测试替代。

优先使用现有模块和真实 Gateway 路由，不新建重复 Agent 服务。将现有波形缺陷视为可本地修复的依赖工作，而不是永久等待外部小组。阶段 17 先稳定证据接口，阶段 18 再消费该接口；L4 按实际 receipt 和已审阅参考材料的可用性开发服务端协议，缺材料时明确不可用，不生成未经审阅的作业答案。

## 全局约束

- Python >=3.11，运行时仅标准库；SQLite FTS5 课程依赖不变。不依赖 UCAgent 或激活 Conda。
- 新 U2 服务、测试与开发文档位于 sigflowAgent；必要修复保持在原 Core/插件位置；保留 agent、所有此前未提交改动和 stash。
- 不删除无关文件或沙箱外既有文件。测试仅清理自己创建并持有的临时目录，不按固定名字清理用户目录或进程。
- L1–L3 输出、模型预算、learner/project/revision/Job 隔离、执行审批与限额不放宽。不调用真实模型批次冒充教师验收。
- 原始课程/源码/问题和凭据不写入新增检查点或机器验收报告。模型与普通 HTTP 不得提供可信契约、锚点、receipt 签发身份或本机路径。

## 阶段 17：完整波形页与可信读取

文件：core/src/eda-agent-gateway/WaveformService.cpp、GatewayDto.cpp；共享 capabilities 可选字段 schema；sigflowAgent/integration/wave_service_test.cpp、CMakeLists.txt、edu_u2_gateway.cpp；clients/waveform.py、context/waveform.py、新 Python 测试。

- [x] C++ 失败回归复现最后一信号事件/字节截断误标 exact、恰好限额、多信号分页及游标绑定。
- [x] 服务只返回完整信号；不能容纳一条完整信号时 partial，不提供无进展游标；completeness 与 has_more 分开判定，恰好满足限额不算截断。
- [x] 修复查询游标严格解析和范围绑定；capabilities 可选 waveform_query_contract=whole-signals-v1 表示已修复的读取语义。旧 Gateway 缺标记时 U2 继续拒绝真实查询。
- [x] typed WaveformReader 仅从认证 Gateway 取报告/产物/信号/查询，以报告的同一 Job/snapshot 和文件摘要建立锚点；严格 tick/信号/完整性/范围/预算校验，不接受客户端原始页或自造锚点。
- [x] 在可选集成宿主装配真实 VCD 后端，真实 Verilator VCD 运行读取验收；不将单次 eval 当动态激励。
- [x] 产出阶段 17 文档与机器记录，定向测试及独立审查通过后进入阶段 18。

## 阶段 18：学生预测交互

文件：新增 host/prediction.py；host/factory.py、runtime.py、http.py；运行时可信动作适配与宿主测试、真实闭环验收。

- [x] prediction_request 只校验作用域和信号元数据、生成绑定 challenge，不查询观察值。
- [x] prediction_submit 必须包含当前 challenge 和明确二态预期，先验证并记录该预期再取真实波形；比较仅为 matches/differs/inconclusive，引用 artifact/hash/Job/revision/tick/timescale，不推断复位极性或全设计正确。
- [x] 动作事务与重放保持幂等；hint/finish、取消、恢复和版本变化使旧 challenge 失效。重启不盲目重放旧波形结论；旧动作返回当前重验投影。
- [x] 不运行 Job、不调用模型、不升级提示等级；完整性不足保留明确缺口，不补零或猜测结果。
- [x] HTTP 拒绝原始页、锚点、契约、路径等注入；覆盖 learner 隔离、过期/重复/冲突、篡改和 uint64 tick。
- [x] 真实波形预测场景、定向回归、独立审查和阶段文档。

## 后续可实施范围审计

- [x] 审核现有 Gateway receipt 的绑定、挑战、核销幂等契约，补齐可用条件下的 reference_request/reference_confirm；教师审阅参考材料缺失时保持不可见，不以模型生成替代。
- [x] 对八类模式逐项记录真实 detector/已核验样例和不支持项，补齐可确定规则的真实回归；无依据的硬件根因不开放。
- [x] 将已有自动评测、对抗与故障用例做统一可复现验收，更新接口与依赖矩阵，列出仅外部签收可完成的项目。

每一阶段记录先失败后通过的测试、实际工具与模拟边界、源码摘要及剩余依赖。最终集中执行全量回归和开发评测，检查文档链接及仓库无暂存状态。

## 阶段结论与交付边界

阶段 17–19 的本地实现与定向验证完成；最终记录见 verification-stage17-19.json。后续 receipt 条目完成范围为严格 Core 核销、协议准备及保持缺材料时不可用，不代表参考答案已发布。八类条目完成范围为真实支持矩阵与可确定的采样比较，不代表八类根因均支持。冻结输入校验仅用于自动 probe，团队冻结 ModelCase、双教师评分与模型语义质量仍待签收。

L4 的持久核销/发布恢复事务在真实审阅材料接入前仍待实现和单独审查；动态 stimuli、GUI、原生目标 OS 和其他 detector 的证据生产需独立交付。当前报告不将这些事项归入已完成能力。
