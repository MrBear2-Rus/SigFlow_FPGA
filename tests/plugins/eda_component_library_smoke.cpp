#include "BasicComponentLibrary.h"

#include <filesystem>
#include <iostream>
#include <string>

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

} // namespace

int main(int argc, char** argv) {
    const fs::path libraryFile = argc > 1 ? argv[1] : "";
    eda::lib::BasicComponentLibrary library;
    std::string error;
    Check(eda::lib::BasicComponentLibrary::LoadFile(libraryFile, library, error),
          "loads canvas_elements.json");
    Check(library.Components().size() >= 4, "library has multiple components");

    const eda::ComponentTemplate* andGate = library.Find("and");
    Check(andGate != nullptr, "finds 'and' gate");
    if (andGate != nullptr) {
        Check(andGate->inputPins.size() == 2, "and gate has 2 input pins");
        Check(andGate->outputPins.size() == 1, "and gate has 1 output pin");
        Check(andGate->width == 80 && andGate->height == 80, "and gate bounds");
        bool hasArc = false;
        for (const auto& shape : andGate->shapes) {
            if (shape.kind == "ArcShape") hasArc = true;
        }
        Check(hasArc, "and gate has an arc shape");
    }

    const eda::ComponentTemplate* nandGate = library.Find("nand");
    Check(nandGate != nullptr, "finds 'nand' gate");
    if (nandGate != nullptr) {
        bool hasCircle = false;
        for (const auto& shape : nandGate->shapes) {
            if (shape.kind == "circle") hasCircle = true;
        }
        Check(hasCircle, "nand gate has a circle shape");
    }

    Check(library.Find("definitely-missing") == nullptr, "missing component returns null");

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
