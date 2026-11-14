// Executes the UNCHANGED production parser/validator/generator to measure K1 gaps.
// A successfully measured gap is not a passing K1 implementation test.
#include "ConstraintSheet.h"
#include <eda/api/Types.h>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    eda::Json fixtures;
    std::ifstream input(argv[1]);
    input >> fixtures;
    eda::target::PinDatabase pins;
    pins.device = "probe-only";
    pins.ioTypes = {"LVCMOS18"};
    eda::target::PinEntry pin;
    pin.pin = 10; pin.available = true;
    pins.pins.push_back(pin);
    eda::cst::ConstraintValidator validator(pins);
    eda::cst::CstGenerator generator;
    eda::Json results = eda::Json::array();
    for (const auto& fixture : fixtures.at("cases")) {
        eda::Json bits = eda::Json::array();
        for (int i = 0; i < fixture.at("width").get<int>(); ++i) bits.push_back(i + 2);
        eda::Json port = {{"direction", "output"}, {"bits", bits},
                         {"offset", fixture.at("offset")}, {"upto", fixture.at("upto")}};
        eda::Json netlist = {{"modules", {{"top", {{"ports", {{"led", port}}}}}}}};
        std::string error;
        auto parsed = validator.ParsePortsFromJsonString(netlist.dump(), "top", error);
        eda::Json result = {{"case", fixture.at("id")}, {"parse_error", error},
                            {"parsed_width", parsed.at("led").width},
                            {"shape_metadata_retained", false}};
        if (fixture.contains("expected_indices")) {
            eda::cst::ConstraintSheet sheet;
            eda::cst::PinBinding binding;
            binding.portName = "led";
            binding.bitIndex = fixture.at("expected_indices")[0].is_null()
                ? 0 : fixture.at("expected_indices")[0].get<int>();
            binding.packagePin = 10; binding.ioType = "LVCMOS18";
            sheet.bindings.push_back(binding);
            auto generated = generator.Generate(sheet);
            const auto expectedRef = fixture.at("expected_refs")[0].get<std::string>();
            result["expected_first_ref"] = expectedRef;
            result["generated_cst"] = generated.cstContent;
            result["first_ref_preserved"] = generated.cstContent.find("IO_LOC \"" + expectedRef + "\"") != std::string::npos;
            auto validation = validator.Validate(sheet, parsed);
            result["validation_errors"] = eda::Json::array();
            for (const auto& item : validation) result["validation_errors"].push_back(item.message);
        }
        results.push_back(result);
    }
    std::cout << eda::Json{{"stage", "K1-analysis"}, {"production_code_modified", false},
                           {"fixture_kind", "reference cases executed against baseline C++"}, {"cases", results}}.dump(2) << '\n';
}
