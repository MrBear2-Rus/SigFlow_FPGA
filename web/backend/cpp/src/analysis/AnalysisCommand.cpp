#include "AnalysisCommand.h"
#include "VerilogTree.h"
#include "eda-core/JobPaths.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace eda::analysis {
namespace {
constexpr std::size_t kMaxInput = 2 * 1024 * 1024;
constexpr std::size_t kMaxOutput = 900 * 1024;
constexpr std::size_t kMaxNodes = 5000;
constexpr std::size_t kMaxVisited = 250000;
class AnalysisLimitError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
struct Position {
    uint32_t line, column;
};
std::vector<Position> Positions(const std::string& text) {
    // nlohmann validates UTF-8; Tree-sitter offsets are bytes, CodeMirror columns are UTF-16.
    (void)Json(text).dump();
    std::vector<Position> positions(text.size() + 1);
    uint32_t line = 1, col = 1;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        for (std::size_t j = 0; j < n; ++j)
            positions[i + j] = {line, col};
        if (n == 4)
            positions[i + n - 1].column++;
        if (c == '\n') {
            ++line;
            col = 1;
        } else
            col += n == 4 ? 2 : 1;
        i += n;
    }
    positions[text.size()] = {line, col};
    return positions;
}
Json Point(Position p) {
    return {{"line", p.line}, {"col", p.column}};
}
std::string Kind(std::string_view type) {
    if (type == "module_declaration")
        return "module";
    if (type == "module_instantiation" || type == "checker_instantiation")
        return "instance";
    if (type == "ansi_port_declaration" || type == "port_declaration")
        return "port";
    if (type == "net_declaration" || type == "data_declaration")
        return "signal";
    if (type == "always_construct")
        return "always";
    if (type == "continuous_assign")
        return "assign";
    if (type == "generate_region" || type == "loop_generate_construct" ||
        type == "conditional_generate_construct" || type == "case_generate_construct")
        return "generate";
    return {};
}
std::string NodeText(TSNode node, const std::string& source) {
    const auto begin = ts_node_start_byte(node), end = ts_node_end_byte(node);
    if (end < begin || end > source.size() || end - begin > 256)
        return {};
    return source.substr(begin, end - begin);
}
std::string Name(TSNode node, const std::string& kind, const std::string& source) {
    if (kind != "module" && kind != "port" && kind != "signal" && kind != "instance")
        return {};
    std::vector<TSNode> stack{node};
    std::string fallback;
    std::size_t examined = 0;
    while (!stack.empty() && examined++ < 2048) {
        auto current = stack.back();
        stack.pop_back();
        auto type = std::string_view(ts_node_type(current));
        if (type == "simple_identifier" || type == "escaped_identifier") {
            auto name = NodeText(current, source);
            if (kind != "instance")
                return name;
            if (fallback.empty())
                fallback = name;
        }
        if (kind == "instance" && type == "instance_identifier") {
            auto name = NodeText(current, source);
            if (!name.empty())
                return name;
        }
        for (uint32_t i = ts_node_named_child_count(current); i > 0; --i)
            stack.push_back(ts_node_named_child(current, i - 1));
    }
    return fallback;
}
struct Work {
    TSNode node;
    int parent;
};
Json Build(TSNode root, const std::string& source, const std::string& file, bool lint) {
    auto positions = Positions(source);
    Json values = Json::array();
    std::vector<Work> stack{{root, -1}};
    std::size_t visited = 0;
    while (!stack.empty()) {
        if (++visited > kMaxVisited)
            throw AnalysisLimitError("analysis syntax tree exceeds node limit");
        auto [node, parent] = stack.back();
        stack.pop_back();
        int nextParent = parent;
        if (lint) {
            if (ts_node_is_error(node) || ts_node_is_missing(node)) {
                if (values.size() >= kMaxNodes)
                    throw AnalysisLimitError("too many syntax diagnostics");
                auto pos = positions.at(ts_node_start_byte(node));
                std::string message = ts_node_is_missing(node)
                                          ? std::string("missing ") + ts_node_type(node)
                                          : "syntax error";
                values.push_back({{"severity", "error"},
                                  {"code", "SYNTAX"},
                                  {"message", message},
                                  {"file", file},
                                  {"line", pos.line},
                                  {"col", pos.column}});
            }
        } else {
            auto kind = Kind(ts_node_type(node));
            if (!kind.empty()) {
                if (values.size() >= kMaxNodes)
                    throw AnalysisLimitError("too many structure nodes");
                auto start = ts_node_start_byte(node), end = ts_node_end_byte(node);
                auto id = "n" + std::to_string(values.size() + 1);
                values.push_back({{"id", id},
                                  {"kind", kind},
                                  {"name", Name(node, kind, source)},
                                  {"range",
                                   {{"start", Point(positions.at(start))},
                                    {"end", Point(positions.at(end > start ? end - 1 : start))}}},
                                  {"children", Json::array()}});
                nextParent = static_cast<int>(values.size() - 1);
                if (parent >= 0)
                    values[static_cast<std::size_t>(parent)]["children"].push_back(id);
            }
        }
        auto count = lint ? ts_node_child_count(node) : ts_node_named_child_count(node);
        for (uint32_t i = count; i > 0; --i)
            stack.push_back(
                {lint ? ts_node_child(node, i - 1) : ts_node_named_child(node, i - 1), nextParent});
    }
    return lint ? Json{{"schema", "sigflow.lint.v1"}, {"diagnostics", values}}
                : Json{{"schema", "sigflow.structure.v1"}, {"nodes", values}};
}
}  // namespace
int AnalysisCommand(int argc, char** argv, const std::function<void(const Json&)>& emit) {
    try {
        std::map<std::string, std::string> options;
        for (int i = 2; i < argc; ++i) {
            std::string key = argv[i];
            if ((key != "--file" && key != "--project-root" && key != "--data-dir") ||
                options.count(key) || i + 1 == argc || std::string(argv[i + 1]).empty() ||
                std::string(argv[i + 1]).starts_with("--"))
                throw std::invalid_argument("invalid analysis option: " + key);
            options[key] = argv[++i];
        }
        if (!options.count("--file"))
            throw std::invalid_argument("--file is required");
        fs::path file = options.at("--file");
        if (!file.is_absolute())
            throw std::invalid_argument("--file must be absolute");
        bool lint = std::string_view(argv[1]) == "lint";
        if (lint && !options.count("--project-root"))
            throw std::invalid_argument("lint requires --project-root");
        fs::path base = options.count("--project-root") ? fs::path(options.at("--project-root"))
                                                        : file.parent_path();
        if (!base.is_absolute())
            throw std::invalid_argument("--project-root must be absolute");
        base = web::CheckedPath(base, base, true);
        if (!fs::is_directory(base))
            throw std::invalid_argument("project root is not a directory");
        file = web::CheckedPath(base, file, true);
        if (!fs::is_regular_file(file))
            throw std::invalid_argument("source is not a regular file");
        if (fs::file_size(file) > kMaxInput)
            throw AnalysisLimitError("source exceeds 2 MiB");
        std::ifstream input(file, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot read source");
        std::string source((std::istreambuf_iterator<char>(input)), {});
        if (input.bad())
            throw std::runtime_error("cannot read source");
        if (source.size() > kMaxInput)
            throw AnalysisLimitError("source exceeds 2 MiB");
        VerilogTree tree(std::move(source));
        auto relative = lint ? file.lexically_relative(base).generic_string() : std::string();
        auto data = Build(tree.root(), tree.source(), relative, lint);
        Json event = {{"type", "result"}, {"ok", true}, {"data", data}};
        if (event.dump().size() + 1 > kMaxOutput)
            throw AnalysisLimitError("analysis output exceeds limit");
        emit(event);
        return 0;
    } catch (const std::exception& error) {
        const bool limitExceeded = dynamic_cast<const AnalysisLimitError*>(&error) != nullptr;
        emit(Json{{"type", "diagnostic"},
                  {"severity", "error"},
                  {"code", limitExceeded ? "ANALYSIS_LIMIT_EXCEEDED" : "ANALYSIS_FAILED"},
                  {"message", error.what()}});
        return 4;
    }
}
}  // namespace eda::analysis
