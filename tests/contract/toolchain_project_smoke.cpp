#include <eda/api/toolchain.hpp>
#include <eda/api/project.hpp>
#include <eda/api/IPluginInteraction.h>
#include <eda/api/plugin_registry.h>

#include "eda-core/PluginHost.h"
#include "eda-core/Project.h"
#include "eda-core/Toolchain.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool ok, const char* msg) {
    if (ok) {
        std::cout << "  ok: " << msg << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << msg << "\n";
    }
}

class BuiltinPlugin final : public eda::IPluginInteraction {
public:
    void invoke(const eda::MethodCall&, eda::Callback<eda::Error, eda::Json> cb) override {
        cb(eda::Error::Ok(), eda::Json::object());
    }
    std::uint64_t subscribe(const std::string&, eda::EventHandler<const eda::Json&>) override {
        return 0;
    }
    void unsubscribe(std::uint64_t) override {}
    eda::PluginInfo info() const override {
        eda::PluginInfo i;
        i.id = "builtin.demo";
        i.capabilities = {"demo/x"};
        i.location = "inner";
        i.runtime = "inprocess";
        return i;
    }
};

} // namespace

EDA_REGISTER_PLUGIN(BuiltinPluginDemo, BuiltinPlugin, "builtin.demo");

int main() {
#ifdef _WIN32
    const std::string suffix = ".exe";
#else
    const std::string suffix = "";
#endif

    {
        const fs::path root = fs::temp_directory_path() / "eda_toolchain_test";
        std::error_code cleanupError;
        fs::remove_all(root, cleanupError);
        fs::create_directories(root / "runtime" / "yosys" / "bin");
        std::ofstream(root / "runtime" / "yosys" / "bin" / ("yosys" + suffix)).put('x');
        const fs::path toolPath = root / "runtime" / "yosys" / "bin" / ("yosys" + suffix);

        eda::DefaultToolchain toolchain;
        eda::ToolQuery query;
        query.name = "yosys";
        query.bundledRoots = {root / "runtime" / "yosys"};
        const eda::ToolResolution bundled = toolchain.Resolve(query);
        Check(bundled.found && bundled.source == "bundled", "toolchain resolves bundled tool");

        eda::ToolQuery configuredQuery = query;
        configuredQuery.configuredPath = toolPath.string();
        const eda::ToolResolution configured = toolchain.Resolve(configuredQuery);
        Check(configured.found && configured.source == "configured",
              "configured path takes precedence");

        eda::DefaultToolchain envToolchain({}, [&](const std::string& name) -> std::string {
            return name == "SIGFLOW_TEST_YOSYS" ? toolPath.string() : std::string();
        });
        eda::ToolQuery envQuery;
        envQuery.name = "yosys";
        envQuery.environmentVariables = {"SIGFLOW_TEST_YOSYS"};
        const eda::ToolResolution fromEnv = envToolchain.Resolve(envQuery);
        Check(fromEnv.found && fromEnv.source == "env", "toolchain resolves environment variable");

        fs::create_directories(root / "pathbin");
        std::ofstream(root / "pathbin" / ("yosys" + suffix)).put('x');
        eda::DefaultToolchain pathToolchain({}, [&](const std::string& name) -> std::string {
            return name == "PATH" ? (root / "pathbin").string() : std::string();
        });
        eda::ToolQuery pathQuery;
        pathQuery.name = "yosys";
        const eda::ToolResolution fromPath = pathToolchain.Resolve(pathQuery);
        Check(fromPath.found && fromPath.source == "path", "toolchain resolves PATH entry");

        eda::ToolQuery missingQuery;
        missingQuery.name = "definitely-missing-tool";
        const eda::ToolResolution missing = toolchain.Resolve(missingQuery);
        Check(!missing.found, "missing tool is not found");
        Check(missing.reason.find("not found") != std::string::npos,
              "missing tool carries explicit reason (R19)");

        // bundled subdirectory（apicula/Scripts 布局，与 MainFrame 的随包发现一致）
        fs::create_directories(root / "runtime" / "apicula" / "Scripts");
        std::ofstream(root / "runtime" / "apicula" / "Scripts" / ("gowin_pack" + suffix)).put('x');
        eda::ToolQuery subQuery;
        subQuery.name = "gowin_pack";
        subQuery.bundledRoots = {root / "runtime"};
        subQuery.bundledSubdirectories = {fs::path("apicula") / "Scripts"};
        const eda::ToolResolution sub = toolchain.Resolve(subQuery);
        Check(sub.found && sub.source == "bundled", "toolchain resolves bundled subdirectory");

        fs::remove_all(root, cleanupError);
    }

    {
        const fs::path dir = fs::temp_directory_path() / "eda_project_test";
        std::error_code cleanupError;
        fs::remove_all(dir, cleanupError);
        fs::create_directories(dir / "rtl");
        {
            std::ofstream out(dir / "sigflow.project");
            out << R"({"build":{"top_module":["top"]},)"
                   R"("paths":{"source_files":["rtl/top.v"],"library_files":[]},)"
                   R"("fpga":{"target_profile":"tang-nano-9k"}})";
        }

        eda::JsonProject project;
        std::string error;
        Check(project.Load(dir / "sigflow.project", error), "project loads");
        Check(project.TopModule() == "top", "project top module");
        Check(project.SourceFiles().size() == 1, "project source files");
        Check(project.AddSourceFile("rtl/uart.v", error), "add source file");
        Check(project.AddSourceFile("rtl/uart.v", error), "add source file is idempotent");
        Check(project.SourceFiles().size() == 2, "source count after add");
        Check(project.Save(error), "project save is atomic");
        Check(!fs::exists(fs::path((dir / "sigflow.project").string() + ".tmp")),
              "no temporary file left behind");

        eda::JsonProject reloaded;
        Check(reloaded.Load(dir / "sigflow.project", error), "project reloads");
        Check(reloaded.SourceFiles().size() == 2, "reloaded source files");
        std::ifstream in(dir / "sigflow.project");
        const std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        Check(content.find("tang-nano-9k") != std::string::npos,
              "unknown fpga section preserved");
        Check(reloaded.RemoveSourceFile("rtl/uart.v", error), "remove source file");
        Check(reloaded.SourceFiles().size() == 1, "source count after remove");

        fs::remove_all(dir, cleanupError);
    }

    {
        eda::PluginHost host;
        Check(host.RegisterBuiltins() >= 1,
              "RegisterBuiltins picks up EDA_REGISTER_PLUGIN entries");
        Check(host.Get("builtin.demo") != nullptr, "registered builtin is retrievable");
        Check(host.ProvidersOf("demo/x").size() == 1, "registered builtin exposes capability");
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
