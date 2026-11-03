#include "analysis/VerilogTree.h"
#include <stdexcept>
#include <string>

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main() {
    eda::analysis::VerilogTree valid("// 中文\nmodule top(input a); endmodule\n");
    auto root = valid.root();
    Check(!ts_node_has_error(root), "valid source has errors");
    Check(ts_node_named_child_count(root) == 2, "expected comment and module");
    auto module = ts_node_named_child(root, 1);
    Check(std::string(ts_node_type(module)) == "module_declaration", "module not found");
    Check(ts_node_start_point(module).row == 1, "incorrect Unicode line location");
    eda::analysis::VerilogTree broken("module top(input a; endmodule\n");
    Check(ts_node_has_error(broken.root()), "syntax error not detected");
    eda::analysis::VerilogTree empty("");
    Check(!ts_node_is_null(empty.root()), "empty source has no tree");
}
