# V1 原始事件库固定源码依赖

这是现有 `ucagent/memory`、版本 `sigflow-ua10-memory==0.1.0` 的固定源码快照，
用于在仅取得本仓库源码时完成安装。Python 源码、概念表和打包声明未作逻辑修改，
逐文件 SHA256 见 PROVENANCE.json；校验仅规范化 Git 的 CRLF/LF 换行差异，
并另存原始 Windows 字节哈希。该发行包只依赖 PyYAML。

UA-10 仍通过发行依赖导入 `memory`，不同时安装或选择两套 V1 实现。
在上层仓库目录运行 `python -m pip install -r requirements-source.txt` 即可。
若宿主已有同版本正式 V1，可继续使用它，但应核对来源和完整性。

本目录不是另起的算法分支。升级应同步原 V1、更新来源校验并运行事件、
删除、迁移和 U2 映射回归。`adapters.py` 中旧 UA-09/UA-11 外部适配器需要
相应宿主模块；默认 UA-10 基础接口不导入这些可选适配器。
