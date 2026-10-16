# DeepSeek 本机凭据与真实测试实施计划

日期：2026-10-15。沿用标准库 DeepSeekProvider，新增本机密钥保存与显式真实请求入口；不提交或推送。

设计：Windows 使用当前用户 DPAPI 加密整个配置，保存于忽略提交的 `.local/deepseek.credentials.json`；不提供明文持久化或命令行 key 参数。非 Windows 保留环境变量，不假称已实现跨平台密钥库。配置使用 getpass 交互输入，拒绝无回显能力时退化为明文输入。状态不解密显示 Key。`DeepSeekProvider.from_env()` 保持原有严格环境语义；新增 from_settings，环境中只要有任一 DeepSeek 配置变量就整套使用环境，不与存储混合，否则读取本机配置。

- [x] 新增 credentials.py：DPAPI 当前用户加密/解密、严格有界配置、原子替换，输出固定错误码。测试真实 Windows roundtrip、磁盘无明文、损坏拒绝、权限来源选择。
- [x] 新增 model_config.py：configure/status/test 三命令；模型名可显式设置，默认 deepseek-flash；test 单次真实请求并经现有卡片检查，失败不规则降级、不自动重试、不显示 Key 或原始服务错误。
- [x] 核心 CLI/宿主/只读工厂接入 from_settings；宿主响应过滤同时覆盖保存的 Key。不改变规则模式、模型预算或现有纯环境接口。
- [x] 测试先失败后实现，运行全量回归及独立审查；产出开发说明、Windows 快捷入口与实际测试结果。没有凭据时明确真实 API 未运行，不用 fixture 代替。

参考：[DeepSeek Chat Completions](https://api-docs.deepseek.com/api/create-chat-completion/)、[Microsoft CryptProtectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)。DPAPI 绑定当前用户，不设置 CRYPTPROTECT_LOCAL_MACHINE；使用禁止系统 UI 的标志。内置测试数据为 fixture，不发送工程文件或执行 EDA。

## 完成结果

配置/测试入口、Windows 快捷命令及开发文档已完成。17 项新增定向测试通过；真实 DPAPI 只使用虚构 Key 在正常用户上下文验证。普通测试已隔离用户保存配置和模型环境变量，审查发现已修复。当前未配置真实 Key，真实 DeepSeek 请求未执行；配置后由显式 test 命令单次调用。未提交或推送。

最终全量复验：385 项，379 通过、6 项历史实验跳过；开发评测 40 通过且源码摘要校验一致。独立审查的凭据隔离发现已复审关闭。4 个文档、64 个本地链接和 6 个 PowerShell 语法块校验通过；Windows 快捷入口 status 在未配置时返回 model_unconfigured/2，未请求 API。
