#include "VerilogStructuring.h"


#include <tree_sitter/api.h>







extern "C" TSLanguage* tree_sitter_verilog();


StructuringReport VerilogStructuring(const std::string& code) {
    StructuringReport rp;

    TSParser* parser = ts_parser_new();
    TSTree* tree = ts_parser_parse_string(parser, NULL, code.c_str(), code.length());


    //struct TOKENS* tokens = lex_code(code.c_str());

    return rp;
}
