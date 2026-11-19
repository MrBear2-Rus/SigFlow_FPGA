# U2 所需运行时依赖设计

日期：2026-10-15。范围：UA-01/02/03/04 中承载 U2 教学核心所需的部分。

## 架构决定

采用 `sigflowAgent/sigflow_edu_agent/host/` 独立宿主，复用 TeachingLoop、TeachingSession、GatewayReportSource、DeepSeekProvider 和只读恢复格式。保留 `agent/` 既有 v1 服务。独立启动器固定自身包路径，避免两个同名 Python 包的导入混用；不将两份服务同时安装到同一个环境。

直接改造旧 RunDispatcher 会扩大冻结接口和教学实现的冲突；跨目录动态加载会使打包依赖源码树布局。因此选择在教学核心内部提供独立宿主，由 C++ 选择对应启动器。基础 health/ready 遵循 sidecar-bootstrap；教学扩展使用 `/api/v1/teaching/`，协议标记 `u2.host.v1`，不冒充已经双方签收的共享 v1 接口。

## 数据与执行边界

- stdin 单行 bootstrap 提供实例与两方向凭据；只绑定 127.0.0.1，Host/Origin/请求尺寸/JSON/路径/动作严格校验。
- session_id 由宿主生成；项目、learner、revision、snapshot、Job、diagnostic、issue 在创建时绑定。凭据不落盘；可信锚点由宿主目录/注入配置提供，HTTP 不接受 Evidence、checkpoint、Provider 或源码路径。
- HTTP 只驱动 advance/host_action。阶段运行在受限后台 worker；长操作返回 202，同项目仅一个活跃运行。响应保留真实来源、证据不足和 state_version。
- SQLite 保存范围、请求摘要、动作指纹、结果、检查点和预算；不保存学生问题、源码、API Key、grant/receipt。模型调用前持久预留预算。pending 操作遇重启不自动重跑。
- 只读检查点恢复后隔离旧卡片，进入 RecoveryRequired；恢复动作需要重新提供原问题并匹配摘要，重新取证后才发布。不能恢复旧执行授权或 L4。
- VerificationPort 只调显式 Gateway 路由，校验详细计划、grant、快照、步骤、前序 build 与幂等结果；提交回应丢失进入待核对，不自动重复提交。
- L4 和精确波形真实路径继续按现有外部依赖拒绝；运行时完善不等于 S1/S2/S3 的数据和授权语义已经通过联合验收。

## 验收分类

单元测试验证作用域、版本、重复动作、预算、磁盘失败和提交不确定性。HTTP 集成测试使用真实 loopback 服务与明确 fixture 来源的 Gateway；进程测试验证 bootstrap/ready/health/关闭与重启。真实 Gateway 探针要求真实实例、Job 和可信锚点，缺输入时明确不执行，不用 skip 冒充通过。真实模型调用需独立配置，不在普通测试中消耗额度。

不提交、不推送；不清理工作区外文件。阶段报告采用开发文档口吻。
