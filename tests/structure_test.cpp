#include "source_graph.h"
#include "structure.h"
#include "symbol_graph.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check_folder_layout()
{
    std::vector<GraphFile> files;
    for (const char* directory : {"a", "b", "c", "d", "e", "f"})
        files.push_back({std::string(directory) + "/file.h", "", {320, 160}});
    const auto initial = folder_layout(files, {});
    const auto refined = folder_layout(files, {{0, 5, 0}});
    expect(initial.files[0].origin.x != refined.files[0].origin.x ||
               initial.files[0].origin.y != refined.files[0].origin.y,
           "New dependencies must influence the placement of folders");
    for (std::size_t i = 0; i < refined.files.size(); ++i)
        for (std::size_t j = i + 1; j < refined.files.size(); ++j)
            expect(!refined.files[i].intersects(refined.files[j]),
                   "Refining the folder layout must keep cards separate");
}

lsp::Json symbol_location(const std::string& uri, int line, int column)
{
    return {{"uri", uri}, {"range", {{"start", {{"line", line}, {"character", column}}}}}};
}

void check_symbol_connections()
{
    SymbolGraph graph;
    graph.reset({"caller", "callee"});
    graph.update("caller", 2,
                 {{0, 4, 6, "function", 1},
                  {1, 8, 6, "function", 0},
                  {2, 3, 4, "variable", 0},
                  {3, 0, 5, "comment", 0}},
                 1);
    const auto call = graph.next();
    expect(call && call->line == 1 && call->begin == 8 && call->end == 14,
           "Only symbol uses should be resolved, retaining the use's byte range");
    graph.started(10, *call);
    const auto variable = graph.next();
    expect(variable && variable->line == 2 && !graph.next(),
           "Data references should be candidates without resolving comments or declarations");
    graph.started(11, *variable);
    const lsp::Json link{{"targetUri", "callee"},
                         {"targetRange", {{"start", {{"line", 3}, {"character", 0}}}}},
                         {"targetSelectionRange", {{"start", {{"line", 4}, {"character", 7}}}}}};
    expect(graph.complete(
               {"textDocument/definition", "caller", 10, lsp::Json::array({link, link}), {}, 2}),
           "Background definition responses should be consumed separately from navigation");
    expect(graph.connections().size() == 1 && graph.connections()[0].line == 4 &&
               graph.connections()[0].column == 7 && graph.connections()[0].source.end == 14,
           "LocationLinks must select the symbol name, deduplicate results and retain use sites");
    graph.complete(
        {"textDocument/definition", "caller", 11, symbol_location("caller", 0, 0), {}, 2});
    expect(graph.connections().size() == 1,
           "Local variable uses must not obscure the logical graph with data arrows");
    expect(!graph.complete({"textDocument/definition", "caller", 99, link, {}, 2}),
           "Interactive navigation requests must remain untouched");
}

void check_symbol_refresh()
{
    SymbolGraph graph;
    graph.reset({"caller", "callee"});
    const std::vector<lsp::SemanticToken> tokens{{2, 0, 4, "method", 0}};
    graph.update("caller", 1, tokens, 0);
    graph.started(1, *graph.next());
    graph.update("caller", 2, tokens, 0);
    graph.complete(
        {"textDocument/definition", "caller", 1, symbol_location("callee", 8, 3), {}, 1});
    expect(graph.connections().empty(), "Edits must invalidate in-flight symbol resolutions");
    graph.started(2, *graph.next());
    graph.complete(
        {"textDocument/definition", "caller", 2, symbol_location("callee", 9, 3), {}, 2});
    expect(graph.connections().size() == 1 && graph.connections()[0].line == 9,
           "Refreshed uses must attach to the current definition line");
    graph.update("caller", 2, tokens, 0);
    graph.started(3, *graph.next());
    graph.reset({"caller"});
    graph.complete(
        {"textDocument/definition", "caller", 3, symbol_location("callee", 9, 3), {}, 2});
    expect(graph.connections().empty() && graph.idle(),
           "Deleting a file must clear its arrows and reject late target locations");
    graph.update("caller", 2, tokens, 0);
    graph.started(4, *graph.next());
    graph.complete(
        {"textDocument/definition", "caller", 4, symbol_location("external", 1, 0), {}, 2});
    expect(graph.connections().empty(),
           "Definitions outside the workspace have no canvas endpoint");
}

void check_symbol_request_budget()
{
    SymbolGraph graph;
    graph.reset({"caller"});
    std::vector<lsp::SemanticToken> tokens;
    for (int line = 0; line < 20; ++line)
        tokens.push_back({line, 0, 4, "function", 0});
    graph.update("caller", 1, tokens, 0);
    for (int id = 1; id <= 8; ++id) {
        const auto query = graph.next();
        expect(query.has_value(), "The initial background batch should be available");
        graph.started(id, *query);
    }
    expect(!graph.next(), "Symbol queries must have a bounded number of in-flight requests");
    graph.complete({"textDocument/definition", "caller", 1, {}, "Unavailable", 1});
    expect(graph.next().has_value(), "Failed queries must release their slot for later uses");
    graph.reset({"caller"});
    graph.update("caller", 2, tokens, 0);
    graph.started(9, *graph.next());
    expect(!graph.next(), "Refreshing the graph must still respect outstanding requests");
}

int main()
{
    try {
        check_folder_layout();
        check_symbol_connections();
        check_symbol_refresh();
        check_symbol_request_budget();
        std::vector<FoldBlock> folds{{1, 8, true}, {3, 5, true}, {10, 13, false}};
        const auto view = fold_view(15, folds);
        expect(view.source_lines == std::vector<int>({0, 1, 9, 10, 11, 12, 13, 14}),
               "Nested collapsed regions must hide their bodies");
        expect(view.display_lines[5] == 1 && view.display_lines[10] == 3,
               "Source positions must map to visible rows");
        expect(reveal_line(4, folds) && !folds[0].collapsed && !folds[1].collapsed,
               "Navigation must reveal every ancestor fold");
        const lsp::Json symbols = lsp::Json::array(
            {{{"name", "Camera"},
              {"kind", 5},
              {"range",
               {{"start", {{"line", 0}, {"character", 0}}},
                {"end", {{"line", 10}, {"character", 1}}}}},
              {"selectionRange",
               {{"start", {{"line", 0}, {"character", 7}}},
                {"end", {{"line", 0}, {"character", 13}}}}},
              {"children", lsp::Json::array({{{"name", "zoom"},
                                              {"kind", 6},
                                              {"range",
                                               {{"start", {{"line", 3}, {"character", 0}}},
                                                {"end", {{"line", 5}, {"character", 1}}}}},
                                              {"selectionRange",
                                               {{"start", {{"line", 3}, {"character", 4}}},
                                                {"end", {{"line", 3}, {"character", 8}}}}}}})}}});
        const auto tree = symbol_tree(symbols);
        expect(tree.size() == 1 && tree[0].children.size() == 1,
               "Symbol hierarchy must preserve classes and methods");
        expect(symbol_definition(tree, 3, 6) && !symbol_definition(tree, 4, 6),
               "Definition clicks must distinguish declarations from uses");
        const std::vector<GraphFile> files{
            {"main.cpp", "#include \"api.h\"\n#include \"api.h\"\n", {800, 500}},
            {"api.h", "", {700, 400}},
            {"tests/test.cpp", "// #include \"missing.h\"\n#include \"../api.h\"\n", {750, 450}},
            {"CMakeLists.txt", "add_executable(app main.cpp api.h)\n", {700, 300}}};
        const auto edges = file_dependencies(files);
        expect(edges.size() == 4 && edges[0].source == 0 && edges[0].target == 1 &&
                   edges[1].target == 1,
               "Dependency extraction must resolve relative paths and deduplicate includes");
        expect(edges[0].line == 0 && edges[0].byte == std::string("#include \"api.h\"").size() &&
                   edges[1].line == 1 &&
                   edges[1].byte == std::string("#include \"../api.h\"").size(),
               "Include endpoints must retain the actual reference line and byte position");
        expect(edges[2].build && edges[3].build && edges[2].byte == 33 && edges[3].byte == 27,
               "Build dependencies on one line must have distinct filename endpoints");
        const auto directory = std::filesystem::temp_directory_path() /
                               ("source-graph-test-" + std::to_string(getpid()));
        std::filesystem::create_directories(directory);
        const auto graph = graph_layout(files, edges, directory.string());
        expect(graph.files.size() == files.size() && graph.routes.size() == edges.size(),
               "Graphviz must place every file and route every dependency");
        for (std::size_t i = 0; i < graph.files.size(); ++i)
            for (std::size_t j = i + 1; j < graph.files.size(); ++j)
                expect(!graph.files[i].intersects(graph.files[j]),
                       "Force-directed layout must avoid overlapping files");
        std::filesystem::remove_all(directory);
        std::cout << "Graph layout, dependency, folding and symbol hierarchy checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
