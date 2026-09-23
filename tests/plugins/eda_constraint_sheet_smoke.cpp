#include "ConstraintSheet.h"
#include "CstFileValidator.h"
#include "PinDatabaseStore.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
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

bool HasErrorMentioning(const std::vector<eda::cst::ConstraintError>& errors,
                        const std::string& needle, bool wantError) {
    for (const auto& error : errors) {
        if (error.IsError() != wantError) continue;
        if (error.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    const fs::path pinsFile = argc > 1 ? argv[1] : "";
    eda::target::PinDatabase database;
    std::string error;
    Check(eda::target::PinDatabaseStore::LoadFile(pinsFile, database, error), "load pin database");

    eda::cst::ConstraintValidator validator(database);

    const std::string netlist =
        R"({"modules":{"top":{"ports":{)"
        R"("clk":{"direction":"input","bits":[0,1]},)"
        R"("led":{"direction":"output","bits":[2,3,4]},)"
        R"("rst":{"direction":"input","bits":[5,6]}}}}})";
    const eda::cst::PortMap ports = validator.ParsePortsFromJsonString(netlist, "top", error);
    Check(ports.size() == 3, "parses three ports from Yosys JSON");
    Check(ports.count("led") == 1 && ports.at("led").width == 3, "led width is 3");
    Check(ports.count("clk") == 1 && ports.at("clk").direction == eda::cst::PortDirection::Input,
          "clk direction is input");

    eda::cst::ConstraintSheet sheet;
    sheet.topModule = "top";
    sheet.device = database.device;
    sheet.targetProfileId = "tang-nano-9k";
    {
        eda::cst::PinBinding clk;
        clk.portName = "clk";
        clk.packagePin = 52;
        sheet.bindings.push_back(clk);
        eda::cst::PinBinding led0;
        led0.portName = "led";
        led0.bitIndex = 0;
        led0.packagePin = 10;
        sheet.bindings.push_back(led0);
        eda::cst::PinBinding led1;
        led1.portName = "led";
        led1.bitIndex = 1;
        led1.packagePin = 11;
        sheet.bindings.push_back(led1);
        eda::cst::PinBinding unknown;
        unknown.portName = "rst";
        unknown.packagePin = 7;  // GND，保留引脚
        unknown.ioType = "BOGUS_IO";
        sheet.bindings.push_back(unknown);
    }

    const std::vector<eda::cst::ConstraintError> errors = validator.Validate(sheet, ports);
    Check(HasErrorMentioning(errors, "only 2 bits are bound", true),
          "detects incomplete vector binding");
    Check(HasErrorMentioning(errors, "reserved", true), "detects reserved pin");
    Check(HasErrorMentioning(errors, "BOGUS_IO", false), "unknown IO_TYPE is a warning");

    // 引脚重复
    {
        eda::cst::ConstraintSheet duplicate = sheet;
        eda::cst::PinBinding extra;
        extra.portName = "led";
        extra.bitIndex = 2;
        extra.packagePin = 10;  // 与 led0 冲突
        duplicate.bindings.push_back(extra);
        const auto duplicateErrors = validator.Validate(duplicate, ports);
        Check(HasErrorMentioning(duplicateErrors, "assigned to 2 ports", true),
              "detects duplicate pin assignment");
    }

    // 生成 + 导入往返
    {
        eda::cst::ConstraintSheet clean;
        clean.topModule = "top";
        clean.device = database.device;
        clean.targetProfileId = "tang-nano-9k";
        eda::cst::PinBinding clk;
        clk.portName = "clk";
        clk.packagePin = 52;
        clk.ioType = "LVCMOS33";
        clk.drive = "8";
        clean.bindings.push_back(clk);

        eda::cst::CstGenerator generator;
        const eda::cst::CstGenerationResult generated = generator.Generate(clean);
        Check(generated.success, "generates CST");
        Check(generated.cstContent.find("IO_LOC \"clk\" 52;") != std::string::npos,
              "generated IO_LOC line");
        Check(generated.cstContent.find("DRIVE=8") != std::string::npos, "generated DRIVE attribute");
        Check(!generated.bindingsHash.empty(), "generated bindings hash");

        std::vector<std::string> unmanaged;
        const auto imported = generator.ImportCst(generated.cstContent, unmanaged, error);
        Check(imported.size() == 1 && imported[0].portName == "clk" &&
                  imported[0].packagePin == 52 && imported[0].drive == "8",
              "import round-trip");
    }

    // CstFileValidator（自 main/fpga/CstValidator 移入）
    {
        const fs::path dir = fs::temp_directory_path() / "eda_cst_validator_test";
        std::error_code cleanupError;
        fs::remove_all(dir, cleanupError);
        fs::create_directories(dir);

        const fs::path good = dir / "tangnano9k.cst";
        {
            std::ofstream out(good);
            out << "// comment\nIO_LOC \"clk\" 52;\n"
                   "IO_PORT \"clk\" IO_TYPE=LVCMOS33 DRIVE=8;\n";
        }
        const eda::cst::CstValidationResult goodResult =
            eda::cst::CstFileValidator::Validate(good.string());
        Check(goodResult.valid && goodResult.syntaxOk, "validates a good CST file");
        Check(goodResult.ioLocCount == 1 && goodResult.ioPortCount == 1, "CST statement counts");

        const fs::path bad = dir / "bad.cst";
        { std::ofstream(bad) << "IO_LOC clk 52;\n"; }
        const eda::cst::CstValidationResult badResult =
            eda::cst::CstFileValidator::Validate(bad.string());
        Check(!badResult.valid && badResult.invalidLines >= 1, "rejects invalid CST line");

        const eda::cst::CstValidationResult coverage =
            eda::cst::CstFileValidator::Validate(good.string(), {"clk", "led"});
        Check(!coverage.valid, "detects unbound port coverage");

        fs::remove_all(dir, cleanupError);
    }

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
