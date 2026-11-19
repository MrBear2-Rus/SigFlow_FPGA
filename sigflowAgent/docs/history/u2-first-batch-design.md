# U2 教学主循环第一阶段设计

日期：2026-09-29。状态：用户已确认主循环优先，并指定 DeepSeek API。

## 已确认的主循环方案

用户确认先做教学主循环，并指定 DeepSeek API。第一批交付 Python >=3.11 的领域核心和离线驱动器，标准库实现，当前机器 Python 3.13。

E0 接收意图 → E1 取证 → E2 检查证据 → E3 教学路径 → E4 检查并发布卡片 → E5 等待学生/批准 → E6 受控验证 → E7 有限结论。

TeachingLoop.step() 单步推进，run_until_pause() 推进到暂停或终态。解释在 E4 完成，诊断等待学生，验证等待授权。模型只生成候选卡片；不能控制阶段、授权、提示级或执行工具。

端口：EvidenceSource.collect(project_id, revision)、CardProvider.generate(context)、VerificationPort.authorized(plan, grant_ref)/execute(plan, step_index, grant_ref, idempotency_key)。默认执行端口拒绝执行，测试替身只用于离线验证。

每次 run 绑定 project/revision/issue；每个验证步骤重查版本和授权。计划只包含目标和最多三个白名单逻辑能力，不包含任意参数/路径，不冒充冻结 HTTP DTO。真实参数、snapshot/grant/plan hash 由 U1/S1 适配器校验。

默认最多 32 阶段步、4 次模型调用。候选不合格最多修正一次，之后规则降级。动作带 action_id/expected_version，当前进程内去重，状态返回深拷贝。取消在阶段边界生效；不支持中断正在等待的同步 HTTP 请求。

DeepSeek 使用 HTTPS Chat Completions + JSON Output，非流式，配置 DEEPSEEK_API_KEY/DEEPSEEK_MODEL；不记录密钥、原始响应或推理内容。没有有效配置可使用规则模式。

已阅读本地 UCAgent VerifyStage、自定义 Checker 和 chat_completions 扩展实现。领域核心后续接入真实 stage；本批不替代上游 StageManager，也不宣称完成真实 UCAgent/EDA 集成。状态暂存内存，服务端持久化、并发和跨重启幂等留给 U1 对接。

验收：解释结束、等待学生/审批、授权后执行、失败停止、旧证据、取消、动作重复、预算、模型修正/降级；DeepSeek 请求构造、响应解析和脱敏；可运行 demo/交互 CLI。模型输出的结构检查不代表语义绝对无泄露，真实教学质量仍需评测和教师审阅。
