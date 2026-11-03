# analysis-v1（structure / lint）草案

状态：**已实现，待双方评审**。2026-11-01 由 js 侧提出；2026-11-02 C++ 实现，
实现选择见 [cpp-implementation-draft.md](cpp-implementation-draft.md)。
产出方：`sigflow-tool structure|lint`；消费方：Node `src/analysis/analysis-runner.js`。

## 调用

```text
sigflow-tool structure --file <绝对路径> [--project-root <dir>] [--data-dir <dir>]
sigflow-tool lint      --file <绝对路径> --project-root <dir> [--data-dir <dir>]
```

## stdout（JSONL）

```text
成功：    {"type":"result","ok":true,"data":{...}}                     退出码 0
失败：    {"type":"diagnostic","severity":"error","code":"…","message":"…"}  退出码 4
未实现：  退出码 3
```

Node 只接受一行 `result` 作为成功结果；出现 `severity=error` 的 diagnostic 或非 0 退出码即失败。
`lint` 必须传工程根（未传返回退出码 4）；Node 侧自动传入。两个参数都必须是绝对路径。

## `structure.data`

```json
{
  "schema": "sigflow.structure.v1",
  "nodes": [
    {
      "id": "m1",
      "kind": "module",
      "name": "top",
      "range": { "start": { "line": 1, "col": 1 }, "end": { "line": 12, "col": 10 } },
      "children": ["i1", "s1"]
    }
  ]
}
```

- `id` 在单次响应内唯一；`children` 引用其他节点 id，根节点不出现在任何 `children` 中。
- `kind` 建议取值：`module` / `instance` / `port` / `signal` / `always` / `assign` / `generate`。
- `range` 的行号、列号均为 **1 起始**，闭区间。

## `lint.data`

```json
{
  "schema": "sigflow.lint.v1",
  "diagnostics": [
    {
      "severity": "error",
      "code": "SYNTAX",
      "message": "unexpected endmodule",
      "file": "src/top.v",
      "line": 12,
      "col": 1
    }
  ]
}
```

- `severity`：`error` / `warning` / `info`。
- `file` 为**工程内相对路径**；`line`、`col` 为 1 起始。

## 未决

- 是否需要跨文件（整个工程）分析，还是只按单文件：当前按单文件设计，`--file` 必填。
- 大文件的截断策略与性能阈值（与 D12 一起定）。
