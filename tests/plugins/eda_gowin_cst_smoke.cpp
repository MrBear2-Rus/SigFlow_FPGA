#include "GowinCst.h"

#include <iostream>
#include <string>
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

const eda::cst::CstBinding* Find(const std::vector<eda::cst::CstBinding>& bindings,
                                 const std::string& name) {
    for (const auto& binding : bindings) {
        if (binding.portName == name) return &binding;
    }
    return nullptr;
}

} // namespace

int main() {
    std::vector<eda::cst::CstBinding> source;
    {
        eda::cst::CstBinding led;
        led.portName = "led";
        led.packagePin = 10;
        led.ioType = "LVCMOS33";
        led.drive = "8";
        source.push_back(led);
        eda::cst::CstBinding clk;
        clk.portName = "clk";
        clk.packagePin = 52;
        clk.ioType = "LVCMOS33";
        clk.pullUp = true;
        source.push_back(clk);
    }

    const std::string generated = eda::cst::GowinCstCodec::Generate(source);
    Check(generated.find("IO_LOC \"led\" 10;") != std::string::npos, "generate emits IO_LOC");
    Check(generated.find("IO_PORT \"led\" IO_TYPE=LVCMOS33 DRIVE=8;") != std::string::npos,
          "generate emits IO_PORT attributes");
    Check(generated.find("PULL_MODE=UP") != std::string::npos, "generate emits pull mode");

    {
        std::vector<eda::cst::CstBinding> parsed;
        std::vector<eda::cst::CstError> errors;
        Check(eda::cst::GowinCstCodec::Parse(generated, parsed, errors), "round-trip parses");
        Check(parsed.size() == 2, "round-trip binding count");
        const auto* led = Find(parsed, "led");
        Check(led != nullptr && led->packagePin == 10 && led->drive == "8",
              "round-trip led fields");
        const auto* clk = Find(parsed, "clk");
        Check(clk != nullptr && clk->packagePin == 52 && clk->pullUp, "round-trip clk fields");
    }

    {
        const std::string text =
            "# comment\n"
            "IO_LOC \"btn\" 4;\n"
            "IO_PORT \"btn\" IO_TYPE=LVCMOS18 DRIVE=4 PULL_MODE=DOWN;\n";
        std::vector<eda::cst::CstBinding> parsed;
        std::vector<eda::cst::CstError> errors;
        Check(eda::cst::GowinCstCodec::Parse(text, parsed, errors), "parse hand-written CST");
        const auto* btn = Find(parsed, "btn");
        Check(btn != nullptr && btn->packagePin == 4 && btn->ioType == "LVCMOS18" &&
                  btn->pullDown,
              "IO_LOC and IO_PORT merged by port name");
    }

    {
        std::vector<eda::cst::CstBinding> parsed;
        std::vector<eda::cst::CstError> errors;
        Check(!eda::cst::GowinCstCodec::Parse("IO_LOC led 10;", parsed, errors),
              "unquoted IO_LOC is an error");
        Check(!errors.empty() && errors[0].IsError(), "error recorded");
    }

    {
        std::vector<eda::cst::CstBinding> parsed;
        std::vector<eda::cst::CstError> errors;
        Check(eda::cst::GowinCstCodec::Parse("SOMETHING else;\n", parsed, errors),
              "unknown statement is only a warning");
        Check(!errors.empty() && !errors[0].IsError(), "warning recorded");
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
