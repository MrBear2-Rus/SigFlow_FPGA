#include <eda/api/IPluginInteraction.h>

#include "eda-core/PluginHost.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

class StaticPlugin final : public eda::IPluginInteraction {
public:
    StaticPlugin(std::string id, std::vector<std::string> capabilities)
        : id_(std::move(id)), capabilities_(std::move(capabilities)) {}

    void invoke(const eda::MethodCall&, eda::Callback<eda::Error, eda::Json> cb) override {
        cb(eda::Error::Ok(), eda::Json::object());
    }
    std::uint64_t subscribe(const std::string&, eda::EventHandler<const eda::Json&>) override {
        return 0;
    }
    void unsubscribe(std::uint64_t) override {}
    eda::PluginInfo info() const override {
        eda::PluginInfo i;
        i.id = id_;
        i.version = "1.0";
        i.capabilities = capabilities_;
        i.location = "inner";
        i.runtime = "inprocess";
        return i;
    }

private:
    std::string id_;
    std::vector<std::string> capabilities_;
};

const eda::PluginRecord* FindRecord(const std::vector<eda::PluginRecord>& records,
                                    const std::string& id) {
    for (const auto& record : records) {
        if (record.id == id) return &record;
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    const std::string pluginDir = argc > 1 ? argv[1] : "";

    // R2：按能力选择（不再按名字硬编码）。
    {
        eda::PluginHost host;
        Check(host.RegisterStatic(std::make_shared<StaticPlugin>(
                  "aaa", std::vector<std::string>{"synth/aaa"})),
              "register static plugin aaa");
        Check(host.RegisterStatic(std::make_shared<StaticPlugin>(
                  "bbb", std::vector<std::string>{"synth/bbb"})),
              "register static plugin bbb");
        Check(!host.RegisterStatic(std::make_shared<StaticPlugin>(
                  "aaa", std::vector<std::string>{"synth/aaa"})),
              "duplicate static id rejected");
        Check(host.ProvidersOf("synth/aaa").size() == 1, "ProvidersOf filters by capability");
        Check(host.ProvidersOf("synth/bbb").size() == 1, "ProvidersOf matches exact capability id");
        Check(host.ProvidersOf("synth").empty(), "ProvidersOf unknown capability is empty");
        Check(host.Select("synth/bbb") == host.Get("bbb"), "Select returns matching instance");
        Check(host.Select("synth/bbb", "aaa") == host.Get("bbb"),
              "Select prefers a capable plugin over an incapable preferred id");
        Check(host.Get("missing") == nullptr, "Get unknown id returns null");

        // P1-8 选择器语义：某能力下的后端集合 + 默认后端（preferredId 命中优先）。
        const auto providers = host.ProvidersOf("synth/aaa");
        Check(providers.size() == 1 && providers[0].id == "aaa", "provider list for capability");
        // 无 capable preferred 时退回首个就绪后端。
        eda::IPluginInteraction* chosen = host.Select("synth/aaa", "bbb");
        Check(chosen == host.Get("aaa"), "Select falls back to first capable provider");
    }

    // R1/R13：动态发现 + ABI/构建指纹校验 + 结构化诊断。
    {
        eda::PluginHost host;
        host.AddSearchPath("definitely-missing-plugin-dir-xyz");
        host.AddSearchPath(pluginDir);
        const std::size_t discovered = host.Discover();
        Check(discovered >= 3, "Discover finds the test plugin libraries");
        const std::size_t ready = host.LoadAll();
        Check(ready == 1, "exactly one dynamic plugin loads successfully");

        const std::vector<eda::PluginRecord> records = host.Records();
        const eda::PluginRecord* ok = FindRecord(records, "test.ok");
        Check(ok != nullptr && ok->status == eda::PluginStatus::Ready, "test.ok is Ready");
        Check(ok != nullptr && !ok->capabilities.empty(), "test.ok capabilities parsed");

        const eda::PluginRecord* badBuild = FindRecord(records, "test.bad_build");
        Check(badBuild != nullptr && badBuild->status == eda::PluginStatus::Error,
              "bad build fingerprint is rejected");
        Check(badBuild != nullptr &&
                  badBuild->error.find("fingerprint") != std::string::npos,
              "bad build error mentions fingerprint");

        const std::string report = host.DiagnosticReport();
        Check(report.find("not found") != std::string::npos,
              "diagnostics report missing search directory (R1)");
        Check(report.find("handshake") != std::string::npos,
              "diagnostics report ABI handshake rejection (R1/R13)");
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
