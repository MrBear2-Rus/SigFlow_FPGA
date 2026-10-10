# GUI — 界面重设计产物（来自 OpenDesign 项目 "1.0"）

> 导出时间：2026-10-10
> 来源：OpenDesign 项目 `1.0`（设计系统 `apple`）
> 定位：SigFlow 界面改造的**设计依据与验收基准**。
> ⚠️ 这里是**设计产物，不是可运行的 C++ 代码**；C++ 落地实现由工程侧在应用源码中完成，不在本目录。

## 文件

| 文件 | 作用 |
| --- | --- |
| `SigFlow-界面改造SPEC.md` | **验收基准**。token 表、材质规则、组件规格、验收清单。实现与本文冲突时**以本文为准** |
| `改造落地指南.md` | 工程路线 + wxWidgets API 映射 |
| `app.html` | 参考实现（交互实底）。与 `index.html` **互相引用，请成对保留** |
| `index.html` | 模块 01–25 组件规格库 |
| `app.html.artifact.json` / `index.html.artifact.json` | OpenDesign 的产物清单（元数据），保留以备追溯 |

## 怎么用

1. **看效果**：浏览器双击 `app.html` 即可（无需任何依赖）。
2. **做实现**：以 `SigFlow-界面改造SPEC.md` 为准；token 变量名**不得改名**（全表见 SPEC §1）。
3. **红线**（SPEC §0）：无 `backdrop-filter`（只能做近似材质）、信息架构不变、
   禁止硬编码颜色、Terminal 的插件诊断 `[Ready] eda-…` 必须可见。

## 注意

- 本目录**自包含**：SPEC、设计稿、落地指南、两个 HTML 都在这里，浏览器直接打开
  `app.html` 即可，**不依赖任何外部文件**。
- OpenDesign 的内部目录（`.file-versions/`、`.od-frames/`）已剔除 —— 经核对不被两个 HTML 引用。
- 相关的工程说明（原有界面实测结构、主题 token 定义、OD↔OpenCode 协作流程）位于项目仓库的
  `docs/design/` 目录下，属于另一处文档，本目录不重复收录。
