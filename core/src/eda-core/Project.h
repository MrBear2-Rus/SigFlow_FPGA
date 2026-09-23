#pragma once

#include <eda/api/Types.h>
#include <eda/api/project.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace eda {

// 基于 `sigflow.project` 的工程实现：保留未知段（如 fpga），写盘为临时文件 + 原子替换。
class JsonProject final : public IProject {
public:
    bool Load(const std::filesystem::path& projectFile, std::string& error) override;
    bool Save(std::string& error) override;
    bool IsLoaded() const override { return loaded_; }
    std::filesystem::path Path() const override { return path_; }

    std::string TopModule() const override;
    void SetTopModule(const std::string& name) override;

    std::vector<std::filesystem::path> SourceFiles() const override;
    bool AddSourceFile(const std::filesystem::path& file, std::string& error) override;
    bool RemoveSourceFile(const std::filesystem::path& file, std::string& error) override;

    const Json& Document() const { return document_; }

private:
    Json& SourceArray();

    std::filesystem::path path_;
    Json document_ = Json::object();
    bool loaded_ = false;
};

} // namespace eda
