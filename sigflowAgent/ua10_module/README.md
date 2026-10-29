# UA-10 长期学习记忆

事件写入、跨会话历史、混合检索、学习规律、证据与时间门禁。默认离线，无需模型权重或密钥。
Python 3.11+，SQLite 需支持 FTS5。在本目录安装：

```powershell
python -m pip install -r requirements-source.txt
python examples/basic_usage.py --help
python tests/run_tests.py
```

U2 集成复用现有 TeachingMemoryPort / teaching_outbox / learning_history；
宿主只组合 U2TeachingMemoryPort 并注入 Runtime，普通调用不增加模型请求。
TeachingMemory 的三项入口及旧教学事实格式仍兼容。

安装、运行最小 U2 示例、Pattern 共享授权与预算及生产限制见
[U2 集成说明](docs/U2_INTEGRATION.md)。本轮为本地联调，尚未提交或生产发布。
BGE/LightMem 可选，不下载模型，不进入默认教学路径。
