#include "GuiSimulationJobProvider.h"

#include "SimulationEngine.h"
#include "../platform/PlatformPaths.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

GuiSimulationJobProvider::GuiSimulationJobProvider(
    std::shared_ptr<SimulationEngine> engine, bool build)
    : engine_(std::move(engine)), build_(build) {}

std::string GuiSimulationJobProvider::jobType() const {
    return build_ ? "sim.gui.build" : "sim.gui.run";
}

eda::Json GuiSimulationJobProvider::paramsSchema() const {
    return eda::Json{{"type", "object"},
                     {"required", build_ ? eda::Json::array({"top_module", "source_files"})
                                          : eda::Json::array({"top_module", "output_vcd"})}};
}

eda::Json GuiSimulationJobProvider::resultSchema() const {
    return eda::Json{{"type", "object"}};
}

eda::Error GuiSimulationJobProvider::startJob(const eda::JobRequest& request,
                                               eda::JobContext& context) {
    if (!engine_ || !request.params.is_object()) {
        return {eda::ErrorCode::InvalidArgument, "GUI simulation engine unavailable", ""};
    }
    const auto& params = request.params;
    const auto StringParam = [&params](const char* name) -> std::string {
        return params.contains(name) && params[name].is_string()
                   ? params[name].get<std::string>() : std::string();
    };
    const std::string topModule = StringParam("top_module");
    if (topModule.empty()) {
        return {eda::ErrorCode::InvalidArgument, "top_module is required", ""};
    }
    engine_->SetProjectRoot(wxString::FromUTF8(request.projectId.c_str()));
    engine_->SetTopModule(wxString::FromUTF8(topModule.c_str()));
    engine_->BeginCoreJob();
    engine_->SetCompileOutputCallback([&context](const wxString& line, bool isError) {
        context.log(sigflow::platform::Utf8String(line), isError);
    });

    std::atomic<bool> finished{false};
    std::thread cancellationWatcher([&]() {
        while (!finished.load()) {
            if (context.cancelled()) {
                engine_->CancelCompile();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });

    eda::Error result = eda::Error::Ok();
    try {
    if (build_) {
        std::vector<wxString> sourceFiles;
        if (params.contains("source_files") && params["source_files"].is_array()) {
            for (const auto& file : params["source_files"]) {
                if (file.is_string()) sourceFiles.push_back(
                    wxString::FromUTF8(file.get<std::string>().c_str()));
            }
        }
        if (sourceFiles.empty()) {
            result = {eda::ErrorCode::InvalidArgument, "source_files are required", ""};
        } else {
            const SimulationCompileResult compiled = engine_->Compile(
                wxString::FromUTF8(topModule.c_str()), sourceFiles);
            if (!compiled.success) {
                result = {eda::ErrorCode::Internal,
                          sigflow::platform::Utf8String(compiled.errorMessage), ""};
            } else {
                eda::Artifact artifact;
                artifact.id = "sim-dll";
                artifact.path = sigflow::platform::Utf8Path(compiled.dllPath);
                artifact.schema = "sigflow.sim.dll.v1";
                artifact.role = "primary";
                context.registerArtifact(artifact);
                context.progress(100, "GUI simulation model compiled");
            }
        }
    } else {
        const std::string outputVcd = StringParam("output_vcd");
        if (outputVcd.empty()) {
            result = {eda::ErrorCode::InvalidArgument, "output_vcd is required", ""};
        } else {
            const SimulationRunResult run = engine_->RunSimulation(
                wxString::FromUTF8(outputVcd.c_str()));
            if (!run.success) {
                result = {eda::ErrorCode::Internal,
                          sigflow::platform::Utf8String(run.errorMessage), ""};
            } else {
                eda::Artifact artifact;
                artifact.id = "waveform";
                artifact.path = sigflow::platform::Utf8Path(outputVcd);
                artifact.schema = "eda.wave.vcd.v1";
                artifact.role = "primary";
                context.registerArtifact(artifact);
                context.progress(100, "GUI simulation complete");
            }
        }
    }
    } catch (const std::exception& ex) {
        result = {eda::ErrorCode::Internal, ex.what(), ""};
    } catch (...) {
        result = {eda::ErrorCode::Internal, "GUI simulation failed unexpectedly", ""};
    }

    finished.store(true);
    cancellationWatcher.join();
    engine_->SetCompileOutputCallback({});
    if (context.cancelled()) {
        return {eda::ErrorCode::Cancelled, "GUI simulation cancelled", ""};
    }
    return result;
}
