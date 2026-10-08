#pragma once

#include <eda/api/Types.h>

#include <cstddef>
#include <string>

namespace eda {
namespace agent {

// 本机路径裁剪（Agent 边界的一部分）。
//
// 约束：Agent 不得看到本机绝对路径。结构化字段里的路径必须替换为文件名；工具自由文本
// （诊断 summary、日志片段）里由工具自己打印的路径同样要裁剪，否则约束只是"大部分成立"。
//
// 原则：
//   * 只做**可证明**的裁剪：Windows 盘符路径（`C:\...`、`C:/...`）、UNC（`\\server\...`）、
//     以及以常见根前缀开头的 POSIX 路径（`/usr`、`/opt`、`/home`、`/tmp`、`/var`、`/etc`、
//     `/mnt`、`/media`、`/root`、`/dev`、`/proc`、`/sys`、`/workspace`）。
//     普通的 `/api/v1/...` 这类 URL 路径不会被误伤。
//   * 替换为**文件名**而不是删除，调用方必须在结果上显式标记发生过裁剪
//     （`path_redacted` / `file_redacted` / `summary_paths_redacted`），不静默改写事实。

// 单个字符串是否看起来像本机绝对路径。
bool LooksLikeAbsolutePath(const std::string& text);

// 取路径的最后一段（无分隔符时原样返回）。
std::string BasenameOfPath(const std::string& text);

// 文本内联裁剪：把每一处本机绝对路径替换为文件名，返回替换次数。
std::size_t RedactLocalPathsInText(std::string& text);

// 递归裁剪 JSON 值里的字符串（对象/数组任意深度），返回是否发生过替换。
bool RedactAbsolutePathsInJson(Json& value);

} // namespace agent
} // namespace eda
