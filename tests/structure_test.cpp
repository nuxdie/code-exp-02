#include "source_graph.h"
#include "structure.h"

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

void check_enclosing_names()
{
    const std::vector<std::string> cpp{"void Application::draw_chat(double y)",
                                       "{",
                                       "    if (ready) {",
                                       "        // note",
                                       "        renderer->draw(mesh);",
                                       "    }",
                                       "}",
                                       "int total = compute();"};
    expect(enclosing_name(cpp, 4) == "draw_chat",
           "A use inside a block must be named after its function, not the if around it");
    expect(enclosing_name(cpp, 7).empty(), "A top-level line sits in nothing");
    const std::vector<std::string> python{"class Store:", "    def save(self, root):",
                                          "        for turn in turns:", "            write(turn)"};
    expect(enclosing_name(python, 3) == "save" && enclosing_name(python, 1) == "Store",
           "Indentation alone must be enough to name a Python method and its class");
    const std::vector<std::string> rust{"impl Camera {", "    fn zoom_at(&mut self, f: f64) {",
                                        "        self.zoom *= f;", "    }", "}"};
    expect(enclosing_name(rust, 2) == "zoom_at", "A Rust method must be named");
    const std::vector<std::string> test{"    static void test_scroll(Application& app)", "    {",
                                        "        expect(app.scroll(3),",
                                        "               \"must scroll\");", "    }"};
    expect(enclosing_name(test, 3) == "test_scroll",
           "A continued statement must still be named after its function");
}

int main()
{
    try {
        check_folder_layout();
        check_enclosing_names();
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
