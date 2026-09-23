#include "Project.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace eda {
namespace {

bool WriteAtomic(const std::filesystem::path& path, const Json& value, std::string& error) {
    std::error_code errorCode;
    std::filesystem::create_directories(path.parent_path(), errorCode);

    const std::filesystem::path temporary = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
        if (!out) {
            error = "unable to write project file: " + temporary.string();
            return false;
        }
        out << value.dump(2) << "\n";
        if (!out) {
            error = "unable to flush project file: " + temporary.string();
            return false;
        }
    }

    errorCode.clear();
    std::filesystem::rename(temporary, path, errorCode);
    if (errorCode) {
        // Windows 下 rename 到已存在目标会失败：先移除目标再重试。
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        errorCode.clear();
        std::filesystem::rename(temporary, path, errorCode);
    }
    if (errorCode) {
        error = "unable to replace project file: " + errorCode.message();
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    return true;
}

} // namespace

bool JsonProject::Load(const std::filesystem::path& projectFile, std::string& error) {
    std::ifstream input(projectFile, std::ios::binary);
    if (!input) {
        error = "project file not found: " + projectFile.string();
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    Json document;
    try {
        document = Json::parse(content);
    } catch (const std::exception& parseError) {
        error = std::string("invalid project file: ") + parseError.what();
        return false;
    }
    if (!document.is_object()) {
        error = "project file must be a JSON object";
        return false;
    }
    if (!document.contains("build") || !document["build"].is_object()) {
        document["build"] = Json::object();
    }
    if (!document.contains("paths") || !document["paths"].is_object()) {
        document["paths"] = Json::object();
    }
    if (!document["paths"].contains("source_files") || !document["paths"]["source_files"].is_array()) {
        document["paths"]["source_files"] = Json::array();
    }
    path_ = projectFile;
    document_ = std::move(document);
    loaded_ = true;
    return true;
}

bool JsonProject::Save(std::string& error) {
    if (!loaded_) {
        error = "no project loaded";
        return false;
    }
    return WriteAtomic(path_, document_, error);
}

Json& JsonProject::SourceArray() {
    if (!document_.contains("paths") || !document_["paths"].is_object()) {
        document_["paths"] = Json::object();
    }
    if (!document_["paths"].contains("source_files") ||
        !document_["paths"]["source_files"].is_array()) {
        document_["paths"]["source_files"] = Json::array();
    }
    return document_["paths"]["source_files"];
}

std::string JsonProject::TopModule() const {
    if (document_.contains("build") && document_["build"].is_object() &&
        document_["build"].contains("top_module") &&
        document_["build"]["top_module"].is_array() &&
        !document_["build"]["top_module"].empty() &&
        document_["build"]["top_module"][0].is_string()) {
        return document_["build"]["top_module"][0].get<std::string>();
    }
    return {};
}

void JsonProject::SetTopModule(const std::string& name) {
    if (!document_.contains("build") || !document_["build"].is_object()) {
        document_["build"] = Json::object();
    }
    document_["build"]["top_module"] = Json::array({name});
}

std::vector<std::filesystem::path> JsonProject::SourceFiles() const {
    std::vector<std::filesystem::path> files;
    if (document_.contains("paths") && document_["paths"].is_object() &&
        document_["paths"].contains("source_files") &&
        document_["paths"]["source_files"].is_array()) {
        for (const auto& entry : document_["paths"]["source_files"]) {
            if (entry.is_string()) files.emplace_back(entry.get<std::string>());
        }
    }
    return files;
}

bool JsonProject::AddSourceFile(const std::filesystem::path& file, std::string& error) {
    if (!loaded_) {
        error = "no project loaded";
        return false;
    }
    const std::string value = file.generic_string();
    Json& sources = SourceArray();
    for (const auto& entry : sources) {
        if (entry.is_string() && entry.get<std::string>() == value) {
            return true; // 幂等
        }
    }
    sources.push_back(value);
    return true;
}

bool JsonProject::RemoveSourceFile(const std::filesystem::path& file, std::string& error) {
    if (!loaded_) {
        error = "no project loaded";
        return false;
    }
    const std::string value = file.generic_string();
    Json& sources = SourceArray();
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        if (it->is_string() && it->get<std::string>() == value) {
            sources.erase(it);
            return true;
        }
    }
    error = "source file not present in project: " + value;
    return false;
}

} // namespace eda
