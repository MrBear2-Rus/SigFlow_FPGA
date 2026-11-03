#include "VerilogTree.h"
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

extern "C" const TSLanguage* tree_sitter_verilog();
namespace eda::analysis {
VerilogTree::VerilogTree(std::string source) : source_(std::move(source)) {
    if (source_.size() > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("Verilog source exceeds Tree-sitter size limit");
    std::unique_ptr<TSParser, decltype(&ts_parser_delete)> parser(ts_parser_new(),
                                                                  ts_parser_delete);
    if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_verilog()))
        throw std::runtime_error("cannot initialize Verilog parser");
    tree_ = ts_parser_parse_string(parser.get(), nullptr, source_.data(),
                                   static_cast<uint32_t>(source_.size()));
    if (!tree_)
        throw std::runtime_error("cannot parse Verilog source");
}
VerilogTree::~VerilogTree() {
    ts_tree_delete(tree_);
}
TSNode VerilogTree::root() const {
    return ts_tree_root_node(tree_);
}
}  // namespace eda::analysis
