# Agent操作要求
## 红线约束
- 本项目全部内容都只在此目录(web/)下，外面的文件均用于参考，不得修改，不得依赖外面的文件。
- agent不得更改git状态，包括add,commit,push,pull和分支切换等。最多只能查看git状态，如git status,git diff等
- 不得修改本文件
- 遇到文档里没有描述清楚的内容时，请主动向我发出询问，等待我亲自回答后再继续
- 文档只能写在 web/current_status_docs/ 下，并遵守 README.md 的「文档规范」：
  * js-agent 只维护 js工 的 plan-js.md、handoff-js.md 与 history-js.md，不得改动 cpp工 的文档；cpp-agent工反之;
  * history-js.md 是模块实况：代码行为变化时原地改写对应段落，不按计划或日期追加；
  * 不得新建其他 .md（backend/cpp/contracts/ 与 backend/cpp/third_party/ 的说明除外）；
  * status 文档里不得复述架构与协议，只能链接 README.md 或 contracts/；
  * 每次提交前更新所负责文档的「更新」日期与相关条目。
## 项目目标
阅读此文件以明确现阶段的目标:[README](./README.md)
具体要做什么请阅读web/current_status_docs下的内容。你需要明确你是cpp工还是js工。


