# 阶段 9：只读联调入口实施计划

目标：将阶段 8 的报告读取接口接入现有 probe，提供可重复运行的命令和稳定摘要。
范围依据：task.md 中真实只读联调准备工作；延续用户继续实现、不提交、不推送的约束。

## 设计

扩展 clients/probe.py，保留无参数能力探测行为；增加 --report 模式，绑定材料全部从环境变量读取。
相比独立重复脚本或直接扩展教学 CLI，该方案复用既有客户端，且不混淆 fixture 教学与真实报告读取。
成功只输出固定 JSON 摘要，不输出诊断正文、路径或 token。失败输出固定错误码到 stderr。
退出 0 仅表示绑定报告读取成功，不能推导证据有效、Job 成功或执行获准。
不增加依赖，不发起 DeepSeek 请求，不创建 Job，不写报告文件。

## 实施步骤

- [x] 新增 probe 测试：缺配置、无效绑定、成功摘要、过期绑定、错误脱敏、原能力模式。
- [x] 运行测试观察缺失行为，再实现 --report 与 --help。
- [x] 使用本地 HTTP 服务验证完整命令入口，运行全量核心及可选上游测试。
- [x] 产出阶段报告，检查文档链接，更新 task.md 再规划。

文件：clients/probe.py、tests/test_gateway_probe.py、docs/stage-9-probe.md、README.md、docs/task.md。
真实服务验收需要宿主提供实例、token 与现有 Job 绑定；本地 HTTP fixture 不替代该验收。
