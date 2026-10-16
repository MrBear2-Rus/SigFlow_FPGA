# D6：可信锚点动态目录

## 接口与生命周期

`host.catalog.AnchorCatalog(path).snapshot()` 返回 `tuple[EvidenceAnchor, ...]`。每次调用重新打开宿主指定的文件，最多读取 64 KiB 加一个越界检测字节，完整解析和验证后才返回快照。目录最多包含 256 个锚点，不保存上次成功读取的内容。

`host.factory.load_anchors(path)` 保持原导入位置可用，执行一次相同的读取与验证；`load_anchors(None)` 返回空元组。需要动态更新时，应将 `AnchorCatalog` 对象直接传入 `make_factory` 与 `make_report_resolver`。传入普通序列时，工厂仍复制为静态元组，调用方随后修改列表不会改变既有配置。

```python
from sigflow_edu_agent.host.catalog import AnchorCatalog
from sigflow_edu_agent.host.factory import make_factory, make_report_resolver

catalog = AnchorCatalog(trusted_catalog_path)
catalog.snapshot()  # 启动前检查；返回值不作为运行时配置。
factory = make_factory(gateway, anchors=catalog)
resolve_result = make_report_resolver(gateway, catalog)
```

工厂创建的会话持有固定的项目、修订、Job、快照和诊断绑定。每次 `collect` 从当前目录读取一次完整快照，按四元组筛选后交给既有 `GatewayReportSource` 验证报告；不会跨多次目录读取拼接锚点。结果 resolver 每次请求同样重新读取一次目录，再按对应 Job 选择可信锚点。报告自身仍不能提供可信锚点。

## 命令行配置

`--anchors` 优先于 `SIGFLOW_U2_ANCHORS`。配置路径时，CLI 在输出 ready 前校验文件，再把同一个活目录对象传入会话工厂和结果 resolver。未配置路径时继续使用静态空元组。

允许使用 `{"anchors":[]}` 启动宿主。可信生产方之后更新该文件，原会话通过显式 `refresh_evidence` 可以重新取证，无需重建会话或重启宿主。目录更新本身不触发动作，也不创建 Job、恢复 grant 或提交后继步骤。

## 验证与失效规则

沿用已有目录边界：严格 JSON，拒绝重复键、非有限数、未知字段、错误类型、重复证据 ID、过期锚点、错误哈希、跨项目或修订的源码引用、错误行列范围，以及路径形式的标识符。整份目录存在任意无效项即拒绝，不跳过坏项。

文件缺失、读取失败、目录损坏或越界统一产生 `ValueError('invalid_anchor_catalog')`。会话取证边界将目录失败转为 `EvidenceError('unresolved_evidence')`，进入等待证据状态；结果 resolver 保留验证失败，由验证适配器按既有证据失败语义处理。撤回为合法空目录时返回空快照，结果 resolver 返回 `None`；已有 source 不能继续使用此前锚点。

可信生产方宜先写同目录临时文件，再原子替换目录文件，避免读取写入中间态。发生中间态时仍立即拒绝当前读取，不回退到旧快照。一次采集使用已取得的完整快照；替换或撤回在下一次采集生效。

目录路径继续属于可信宿主配置，普通 HTTP 请求不接受目录路径或 `Evidence` 注入。本阶段不实现可信生产方，也不扩大文件系统信任边界。

## 验证记录

新增目录测试初次执行 10 项，8 项因缺少 `AnchorCatalog` 失败，2 项静态兼容测试通过；实现目录和工厂后 10 项通过。新增真实宿主进程测试后执行 11 项，其中 CLI 仍冻结启动快照的测试按预期失败；CLI 接入活目录后 11 项通过。

定向回归：`test_host_catalog.py` 11 项、`test_host_http.py` 16 项、`test_host_process.py` 4 项全部通过。测试覆盖空目录增补、同一会话刷新、目录撤回/损坏/缺失/过期失效、resolver 更新、静态序列兼容、恶意目录、显式参数优先级与进程内实时更新。

上述证据均来自本地 HTTP fixture 和标准库宿主进程，模型调用数为零；不代表真实 Gateway、EDA、DeepSeek 或 Linux 已完成验收。
