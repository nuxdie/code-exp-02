#include "application.h"

#include <unistd.h>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct ApplicationTestAccess {
    static void check(const std::filesystem::path& root, const std::filesystem::path& build,
                      const std::string& file)
    {
        Application app;
        expect(app.initialize(), "Workspace application must initialize");
        expect(app.workspace_root == std::filesystem::canonical(root).string(),
               "Workspace must resolve the launch directory or explicit override");
        expect(app.workspace_build == build.string(),
               "Language tools and generated files must use the selected project");
        expect(app.panels.size() == (file.empty() ? 0 : 1),
               "Canvas must contain only the selected project's source");
        if (!file.empty()) {
            expect(app.panels.front().path == file &&
                       app.panels.front().uri == lsp::file_uri((root / file).string()),
                   "Source cards and navigation must point into the selected project");
            expect(std::filesystem::exists(build / "source-graph.dot"),
                   "Graph layout must be generated inside the selected project");
        }
        expect(app.agent_context({0, 0}).at("workspace") == app.workspace_root,
               "Agent context must describe the selected workspace");
        app.chat->disabled = false;
        app.initialize_reload();
        expect(!app.auto_build && !app.live_build,
               "Editing another project must never rebuild or reload the viewer");
        app.chat->disabled = true;
    }
};

int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto launch_directory = std::filesystem::current_path();
    const auto root =
        std::filesystem::temp_directory_path() / ("canvas workspace " + std::to_string(getpid()));
    int result = 0;
    try {
        std::filesystem::create_directories(root / "other/build");
        std::filesystem::create_directories(root / "empty");
        std::ofstream(root / "hello.py") << "print('launch directory')\n";
        std::ofstream(root / "other/build/compile_commands.json") << "[]\n";
        std::filesystem::current_path(root);
        unsetenv("SOURCE_CANVAS_PROJECT_ROOT");
        unsetenv("SOURCE_CANVAS_AUTO_BUILD");
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_SESSION", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
        ApplicationTestAccess::check(root, root / ".cache/source-canvas", "hello.py");
        setenv("SOURCE_CANVAS_PROJECT_ROOT", "", 1);
        ApplicationTestAccess::check(root, root / ".cache/source-canvas", "hello.py");
        std::ofstream(root / "other/override.cpp") << "int override_value = 42;\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", "other/../other", 1);
        ApplicationTestAccess::check(root / "other", root / "other/build", "override.cpp");
        std::ofstream(root / "other/compile_commands.json") << "[]\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", (root / "other").c_str(), 1);
        ApplicationTestAccess::check(root / "other", root / "other", "override.cpp");
        std::filesystem::current_path(root / "empty");
        unsetenv("SOURCE_CANVAS_PROJECT_ROOT");
        ApplicationTestAccess::check(root / "empty", root / "empty/.cache/source-canvas", "");
        std::cout
            << "Launch directory, overrides, compilation databases and empty workspace passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::current_path(launch_directory);
    std::filesystem::remove_all(root);
    return result;
}
