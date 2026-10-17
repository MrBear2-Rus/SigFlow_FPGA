# DeepSeek 本机密钥配置与真实测试

日期：2026-10-15。实现位于 `credentials.py`、`model_config.py`，复用现有 DeepSeekProvider。Python >=3.11、仅使用标准库，不要求激活 Conda 或安装 UCAgent。

## 快速使用

在仓库根目录的正常 Windows PowerShell 终端执行：

```powershell
.\sigflowAgent\bin\sigflow-deepseek.cmd configure --model deepseek-flash
.\sigflowAgent\bin\sigflow-deepseek.cmd status
.\sigflowAgent\bin\sigflow-deepseek.cmd test
```

configure 会通过隐藏输入提示读取 API Key，输入后按 Enter；不接受 `--key` 参数，不应把 Key 写入命令或文档。终端不能安全隐藏输入时返回 hidden_input_unavailable，不退化为明文输入。configure 不请求网络；重新运行可原子替换配置。

默认模型为 deepseek-flash，也可通过 `--model` 指定账户可用模型。模型与请求参数依据 [DeepSeek Chat Completions 官方文档](https://api-docs.deepseek.com/api/create-chat-completion/)，本入口复用 JSON 输出、非流式、关闭 thinking 的现有适配器。

status 只显示是否配置、有效模型和来源，不显示完整或部分 Key，不调用 API。test 会实际请求 DeepSeek 并消耗账户对应额度：只发送内置教学示例，一次 generate 请求，不自动重试或规则降级，不读取工程文件、不执行 EDA。候选卡片须通过现有等级/结构/证据引用检查才报告 passed；返回文本中出现完整 Key 会被替换。

## 存储与优先级

默认文件：`sigflowAgent/.local/deepseek.credentials.json`，该目录已被 Git 忽略。文件仅含格式标识和加密块；API Key、模型与其他配置整体由 Windows 当前用户 DPAPI 加密，临时文件也只有密文。实现不设置机器范围共享标志，详见 [Microsoft DPAPI 文档](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)。解密仅在当前进程使用，保护范围为 Windows 用户账户，不代表同账户恶意进程无法访问。

可用 `SIGFLOW_DEEPSEEK_CONFIG` 指向可信配置文件路径；此变量不包含 Key。默认文件路径与当前工作目录无关。非 Windows 不提供明文持久化回退，继续使用环境配置；Linux 密钥库未在本阶段实现。

`DeepSeekProvider.from_settings()` 的来源规则：

1. 若进程中存在任一 DEEPSEEK_API_KEY、DEEPSEEK_MODEL、DEEPSEEK_BASE_URL 变量，整套使用环境配置；不与本机存储混合。配置不完整时明确失败。
2. 上述变量都不存在时，读取当前用户加密文件。
3. 缺配置或解密失败不调用网络。既有 `from_env()` 仍严格只读环境，语义不变。

使用保存文件前，如存在旧的环境配置，可在当前终端清除这些变量：

```powershell
Remove-Item Env:DEEPSEEK_API_KEY -ErrorAction SilentlyContinue
Remove-Item Env:DEEPSEEK_MODEL -ErrorAction SilentlyContinue
Remove-Item Env:DEEPSEEK_BASE_URL -ErrorAction SilentlyContinue
.\sigflowAgent\bin\sigflow-deepseek.cmd status
```

`.env` 文件仍不会自动加载。现有教学 CLI、Gateway 会话工厂和宿主已接入 from_settings；规则模式不请求模型。已经启动的宿主在初始化时获取配置，更换 Key 后需重新启动该宿主。持久模型预算、受控执行与原有教学检查不变；宿主响应过滤同时覆盖从文件加载的 Key。

## 单次真实测试结果

成功退出 0，输出 status=passed、producer=model、model、evidence_origin=fixture、requests=1 和经过检查的 card。该结果证明一次 API 请求及候选检查完成，不代表真实工程诊断或教学质量验收。

| 退出码/诊断 | 含义 |
| --- | --- |
| 0 | 配置保存、状态读取或真实卡片测试成功，含义由命令决定 |
| 2 / model_unconfigured | 缺少有效配置，或保存文件不能在当前用户解密 |
| 2 / hidden_input_unavailable | 当前终端不能保证隐藏输入 |
| 2 / key_store_unsupported | 非 Windows 环境不提供此持久密钥库 |
| 2 / key_store_invalid、key_store_unavailable、key_store_write_failed | 格式/密文、系统加密或写入失败；只显示固定错误码 |
| 3 / model_auth_failed、model_rate_limited、model_timeout、model_unavailable | 真实 API 调用失败，不报告规则回退为成功 |
| 3 / invalid_output | API 返回的候选未通过 JSON 或教学卡片检查 |

受限沙箱账户可能没有可用 DPAPI 用户配置，出现 key_store_unavailable 时需在正常 Windows 用户终端验证；不会因此写入明文。普通 unittest 使用虚构 Key 和模拟 API，不消耗真实额度。

## 验证记录

新增 12 项首轮用例因缺少模块失败，实现后 12 项在正常 Windows 用户上下文通过；随后补充原子替换失败保留旧配置、宿主脱敏材料、无效输出拒绝和 Key 回显过滤等回归。

正常 Windows 当前用户上下文全量 385 项：379 项通过、6 项历史可选实验跳过，其中新配置测试 17 项全部通过。涵盖真实 DPAPI 虚构 Key 往返、磁盘无明文、密文篡改、原子写入失败、普通测试隔离开发者凭据、来源优先级、单次模型调用、坏卡片、Key 回显和隐藏输入拒绝。既有 DeepSeek 9 项、Gateway Session 10 项、宿主 HTTP 16 项和 CLI 3 项也通过。

配置功能初始验证阶段未调用真实 API。随后 D11 在正常 Windows 当前用户上下文使用已保存的加密配置，完成真实 Core/Yosys 教学闭环：初始卡 `producer=model`、实际 `model_calls=1`，审批后的新 Job、新证据和反馈均完成。详见 [D11 手册](u1-d11-closed-loop.md)及[脱敏机器报告](closed-loop-deepseek.json)。此结果不代表教师教学质量验收，也不把规则降级记为模型成功。

独立审查确认 DPAPI 调用、原子密文保存、来源优先级及单次调用路径；审查发现的普通 fixture 配置隔离问题已修复。进程与工厂测试显式使用测试专属空配置，不读取开发者加密文件，也不继承模型环境凭据。开发评测结果与源码摘要见 [evaluation-deepseek-config.json](evaluation-deepseek-config.json)。
