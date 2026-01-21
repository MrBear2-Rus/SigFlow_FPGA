#include "AtomsAnalysis.h"
#include <json/json.h>
#include <fstream>
#include <wx/filename.h>

//#include "slang/diagnostics/DiagnosticEngine.h" // 诊断引擎
//#include "slang/text/SourceManager.h"           // 源码管理器

void AtomAnalysis::AnalyzeProject(wxString project_path) {
    // --- 1. JSON 解析 (使用 JsonCpp) ---
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::ifstream ifs(project_path.ToStdString()+ "\\"+ "sigflow.project"); // 确保 #include <fstream>
    if (!Json::parseFromStream(builder, ifs, &root, &errs)) {
        return;
    }

    // --- 2. 映射路径到 SourceLoader ---
    // 根据源码，添加普通源文件的方法是 addFiles (第 18 行)
    if (root["paths"].isMember("source_files")) {
        for (const auto& file : root["paths"]["source_files"]) {
            driver.sourceLoader.addFiles(file.asString());
        }
    }

    // --- 3. 映射 Include 目录到 SourceManager ---
    // SourceManager 并不负责“逻辑”路径，它负责“物理”路径
    // 在 slang 架构中，搜索目录通常通过 addSearchDirectories (第 41 行) 添加
    if (root["paths"].isMember("include_dirs")) {
        for (const auto& dir : root["paths"]["include_dirs"]) {
            driver.sourceLoader.addSearchDirectories(dir.asString());
        }
    }

    // --- 4. 映射 Build 配置到 Options ---
    if (root["build"].isMember("top_module")) {
        driver.options.topModules.push_back(root["build"]["top_module"].asString());
    }

    // --- 5. 执行流程 ---
    if (!driver.processOptions()) return;

    // parseAllSources 内部会自动调用 sourceLoader.loadAndParseSources
    if (driver.parseAllSources()) {
        // createCompilation 返回 unique_ptr<Compilation>
        compilation = driver.createCompilation();

        if (compilation) {
            // 修正：新版获取诊断的接口通常是 getAllDiagnostics()
            auto diags = compilation->getAllDiagnostics();
            if (diags.empty()) {
                wxLogMessage("Slang: Elaboration Successful.");
            }
            else {
                driver.reportDiagnostics(false);
            }
        }
    }
}
