# analysis-v1 C++ 本地实现补充草案

本文记录 C++ 的实现选择，待 JS 与主 contributor 评审；原始需求见 [analysis-v1](README.md)。

```text
sigflow-tool structure --file <绝对路径> [--project-root <dir>] [--data-dir <dir>]
sigflow-tool lint --file <绝对路径> --project-root <dir> [--data-dir <dir>]
```

`--file` 与 `--project-root` 必须为绝对路径。lint 必须传工程根，返回其中的相对文件路径；structure 可不传，传入时同样检查包含关系。拒绝符号链接及 `..` 路径段。Node 当前未传工程根，因此 lint 接入需由 JS 方更新。

结果结构与错误 envelope 沿用原草案。范围为 1 起始、闭区间，列按 UTF-16 代码单元计数；与 CodeMirror 使用的 JavaScript 字符串下标一致。语法错误包含在成功结果的 diagnostics 数组里；读取和解析环境失败使用错误 diagnostic 及退出码 4。

单文件输入最多 2 MiB，遍历最多 250000 个语法节点，输出最多 5000 个结构节点或诊断，一行 JSONL 上限 900 KiB，超限返回错误而不静默截断。该数值为当前实现边界，最终性能要求仍需评审。

lint 基于 Tree-sitter 的语法错误和缺失节点，不提供跨文件或语义检查；structure 提供部分常见语法节点。解析结果不能代替编译或综合。原有语法依赖的上游精确版本还需核验。
