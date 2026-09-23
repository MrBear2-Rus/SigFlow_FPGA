#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace eda {

// 工程模型 + `sigflow.project` 原子更新（修 R9/R17：新增 .v 同步进工程，写盘不可被截断）。
class IProject {
public:
    virtual ~IProject() = default;

    virtual bool Load(const std::filesystem::path& projectFile, std::string& error) = 0;
    virtual bool Save(std::string& error) = 0;   // 临时文件 + 原子替换
    virtual bool IsLoaded() const = 0;
    virtual std::filesystem::path Path() const = 0;

    virtual std::string TopModule() const = 0;
    virtual void SetTopModule(const std::string& name) = 0;

    virtual std::vector<std::filesystem::path> SourceFiles() const = 0;
    virtual bool AddSourceFile(const std::filesystem::path& file, std::string& error) = 0;
    virtual bool RemoveSourceFile(const std::filesystem::path& file, std::string& error) = 0;
};

} // namespace eda
