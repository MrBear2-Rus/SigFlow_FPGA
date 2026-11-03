#pragma once
#include <tree_sitter/api.h>
#include <string>

namespace eda::analysis {
// Owns both source bytes and tree so nodes remain valid for this object's lifetime.
class VerilogTree final {
  public:
    explicit VerilogTree(std::string source);
    ~VerilogTree();
    VerilogTree(const VerilogTree&) = delete;
    VerilogTree& operator=(const VerilogTree&) = delete;
    TSNode root() const;
    const std::string& source() const { return source_; }

  private:
    std::string source_;
    TSTree* tree_ = nullptr;
};
}  // namespace eda::analysis
