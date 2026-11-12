#include "application.h"

#include <unistd.h>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct ApplicationTestAccess {
    static void check_progressive_load(const std::filesystem::path& root)
    {
        const auto fixture = root / "other/build/progressive";
        std::filesystem::create_directories(fixture / "include");
        std::ofstream(fixture / "main.cpp") << "#include \"include/api.h\"\n";
        std::ofstream(fixture / "include/api.h")
            << "#pragma once\n// " << std::string(1200, 'x') << '\n';
        Application app(fixture.string());
        expect(app.initialize_window(), "Window must initialize before loading the project");
        XMapWindow(app.display, app.window);
        XSync(app.display, False);
        expect(app.window && app.panels.empty() && !app.chat,
               "Mapping the window must not build the workspace");
        app.workspace_load = Application::WorkspaceLoad{};
        app.draw();
        app.begin_workspace_load();
        app.workspace_load->reading.wait();
        app.advance_workspace_load();
        expect(app.panels.empty() && app.workspace_load->plan.size() == 2,
               "Every file must have its place before any card is built");
        const auto estimates = app.workspace_load->plan;
        expect(estimates[1].size.y > estimates[0].size.y &&
                   estimates[1].size.x > 4 * estimates[0].size.x,
               "A file's planned card must follow from its line and column counts");
        app.draw();
        app.workspace_load->detail.wait();
        app.advance_workspace_load();
        expect(app.workspace_load && app.panels.size() == 1 && !app.chat,
               "The first file card must be ready before the complete workspace");
        const auto& first = app.panels.front().bounds;
        expect(first.origin.x == estimates[0].origin.x && first.origin.y == estimates[0].origin.y &&
                   std::abs(first.size.y - estimates[0].size.y) < 1 &&
                   std::abs(first.size.x - estimates[0].size.x) < 40,
               "A card must appear where it was planned, at about the planned size");
        const auto& edges = app.workspace_load->edges;
        expect(edges.size() == 1 && edges.front().source == 0 && edges.front().target == 1,
               "A loaded file must expose its dependency on a placeholder");
        app.draw();
        XEvent close{};
        close.xclient.type = ClientMessage;
        close.xclient.message_type = app.protocols;
        close.xclient.format = 32;
        close.xclient.data.l[0] = app.close_message;
        app.handle_event(close);
        expect(!app.running, "The window must accept close while loading");
    }

    static void check(const std::filesystem::path& root, const std::filesystem::path& build,
                      const std::string& file, const std::string& argument)
    {
        Application app(argument);
        expect(app.initialize(), "Workspace application must initialize");
        expect(app.workspace_root == std::filesystem::canonical(root).string(),
               "Workspace must resolve the explicit directory or environment override");
        expect(app.workspace_build == build.string(),
               "Language tools and generated files must use the selected project");
        expect(std::ranges::all_of(app.panels,
                                   [&](const auto& panel) {
                                       return panel.path == file ||
                                              panel.path == "compile_commands.json";
                                   }) &&
                   app.panels.empty() == file.empty(),
               "Canvas must contain only the selected project's files");
        if (!file.empty()) {
            const auto card = std::ranges::find(app.panels, file, &SourcePanel::path);
            expect(card != app.panels.end() && card->uri == lsp::file_uri((root / file).string()),
                   "Source cards and navigation must point into the selected project");
        }
        expect(app.agent_context({0, 0}).at("workspace") == app.workspace_root,
               "Agent context must describe the selected workspace");
    }

    static void check_images(const std::filesystem::path& root)
    {
        std::filesystem::create_directories(root / "assets");
        const SurfaceHandle surface{cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1200, 600)};
        const CairoHandle cr{cairo_create(surface.get())};
        cairo_set_source_rgba(cr.get(), 1, 0, 0, 0.5);
        cairo_paint(cr.get());
        cairo_surface_write_to_png(surface.get(), (root / "assets/photo.PNG").c_str());
        std::ofstream(root / "assets/vector.svg")
            << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80\" height=\"40\">"
               "<rect width=\"80\" height=\"40\" fill=\"blue\"/></svg>";
        std::ofstream(root / "broken.jpg") << "invalid image";
        Application app(root.string());
        expect(app.initialize(), "Image workspace must initialize");
        auto found = std::ranges::find(app.panels, "assets/photo.PNG", &SourcePanel::path);
        expect(found != app.panels.end() && found->image && found->mesh.texture &&
                   found->image_size.x == 640 && found->image_size.y == 320 &&
                   found->source.empty() && found->lines.empty(),
               "Binary images in assets must render with aspect ratio and no text content");
        const auto vector = std::ranges::find(app.panels, "assets/vector.svg", &SourcePanel::path);
        expect(vector != app.panels.end() && vector->mesh.texture && vector->image_size.x == 80,
               "SVG files must render as images instead of XML text");
        app.draw();
        const auto height = found->bounds.size.y;
        app.replace_source(static_cast<int>(found - app.panels.begin()),
                           read_project(root.string()).at("assets/photo.PNG"));
        expect(found->mesh.texture && found->bounds.size.y == height,
               "Live image replacement must retain image geometry");
        const auto broken = std::ranges::find(app.panels, "broken.jpg", &SourcePanel::path);
        expect(broken != app.panels.end() && broken->image && !broken->mesh.texture &&
                   broken->mesh.vertices > 0,
               "Corrupt images must show an error card without failing workspace loading");
        std::filesystem::remove_all(root / "assets");
        std::filesystem::remove(root / "broken.jpg");
    }

    static void check_default()
    {
        Application app;
        expect(app.initialize_window(), "Default application must open from any working directory");
        app.workspace_load = Application::WorkspaceLoad{};
        app.begin_workspace_load();
        app.workspace_load->reading.wait();
        app.advance_workspace_load();
        expect(app.panels.empty() && !app.workspace_load->plan.empty(),
               "Filesystem metadata must lay out cards before reading their contents");
        app.workspace_load->detail.wait();
        app.advance_workspace_load();
        expect(app.workspace_root == std::filesystem::canonical(project_root).string(),
               "Without a directory argument the viewer must open its own source tree");
        expect(app.workspace_build == project_build, "Own source must use the viewer build");
        expect(std::ranges::any_of(app.panels,
                                   [](const auto& panel) { return panel.path == "main.cpp"; }),
               "The default canvas must include the app's main.cpp");
    }

    static void choose(Application& app)
    {
        XEvent key{};
        key.xkey.display = app.display;
        key.xkey.keycode = XKeysymToKeycode(app.display, XK_o);
        key.xkey.state = ControlMask;
        app.handle_key(key);
        expect(app.directory_picker != nullptr, "Ctrl+O must open a directory picker");
        const auto* picker = app.directory_picker.get();
        app.handle_key(key);
        expect(app.directory_picker.get() == picker, "Repeated Ctrl+O must reuse the picker");
        pollfd fd{picker->event_fd(), POLLIN, 0};
        expect(poll(&fd, 1, 5000) == 1, "Directory picker must wake the application on completion");
        app.poll_directory_picker();
        expect(!app.directory_picker, "Finished picker must be released");
    }

    static void check_picker(const std::filesystem::path& root)
    {
        Application app(root.string());
        expect(app.initialize(), "Picker application must initialize");
        const auto* environment_path = std::getenv("PATH");
        const std::string path = environment_path ? environment_path : "";
        setenv("PATH", (root / "empty").c_str(), 1);
        app.open_directory_picker();
        expect(!app.directory_picker && app.popup_text.contains("zenity"),
               "A missing picker must explain which dependency to install");
        setenv("PATH", path.c_str(), 1);
        setenv("PICKER_WAIT", "1", 1);
        const auto start = Application::Clock::now();
        app.open_directory_picker();
        app.directory_picker.reset();
        expect(Application::Clock::now() - start < 2s,
               "Closing an active picker must terminate its process without waiting for input");
        unsetenv("PICKER_WAIT");
        app.open_search();
        setenv("PICKER_CANCEL", "1", 1);
        choose(app);
        expect(app.running && !app.reload_requested && app.search.active,
               "Canceling the picker from search must preserve the workspace");
        unsetenv("PICKER_CANCEL");
        setenv("PICKER_DIRECTORY", (root / "missing").c_str(), 1);
        choose(app);
        expect(app.running && !app.reload_requested && !app.popup_text.empty(),
               "An invalid selection must show an error without leaving the project");
        setenv("PICKER_DIRECTORY", root.c_str(), 1);
        choose(app);
        expect(app.running && !app.reload_requested,
               "Choosing the current directory must stay open");
        setenv("PICKER_DIRECTORY", (root / "other").c_str(), 1);
        app.chat->focused = true;
        choose(app);
        expect(app.reload_requested && !app.running &&
                   app.directory() == std::filesystem::canonical(root / "other").string(),
               "Ctrl+O from an editor must reopen on the chosen directory");
        expect(app.workspace_root == std::filesystem::canonical(root).string(),
               "The old workspace must remain intact while its session and services shut down");
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
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_SESSION", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
        ApplicationTestAccess::check_progressive_load(root);
        std::filesystem::current_path(root / "other/build");
        ApplicationTestAccess::check_default();
        setenv("SOURCE_CANVAS_PROJECT_ROOT", "", 1);
        ApplicationTestAccess::check_default();
        std::filesystem::current_path(root);
        ApplicationTestAccess::check(root, root / ".cache/source-canvas", "hello.py", ".");
        std::ofstream(root / "other/override.cpp") << "int override_value = 42;\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", "other/../other", 1);
        ApplicationTestAccess::check(root / "other", root / "other/build", "override.cpp", "");
        ApplicationTestAccess::check(root / "empty", root / "empty/.cache/source-canvas", "",
                                     (root / "empty").string());
        std::ofstream(root / "other/compile_commands.json") << "[]\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", (root / "other").c_str(), 1);
        ApplicationTestAccess::check(root / "other", root / "other", "override.cpp",
                                     "other/../other");
        std::filesystem::current_path(root / "empty");
        unsetenv("SOURCE_CANVAS_PROJECT_ROOT");
        ApplicationTestAccess::check(root / "empty", root / "empty/.cache/source-canvas", "", ".");
        std::filesystem::create_directories(root / "bin");
        const auto picker = root / "bin/zenity";
        std::ofstream(picker) << "#!/bin/sh\n"
                                 "[ \"$PICKER_CANCEL\" = 1 ] && exit 1\n"
                                 "[ \"$PICKER_WAIT\" = 1 ] && exec /bin/sleep 30\n"
                                 "printf '%s\\n' \"$PICKER_DIRECTORY\"\n";
        std::filesystem::permissions(picker, std::filesystem::perms::owner_all);
        const auto* environment_path = std::getenv("PATH");
        const auto path =
            (root / "bin").string() + ":" + (environment_path ? environment_path : "");
        setenv("PATH", path.c_str(), 1);
        ApplicationTestAccess::check_picker(root);
        ApplicationTestAccess::check_images(root);
        std::cout << "Own source default, directory arguments, overrides, compilation databases, "
                     "empty workspace and Ctrl+O picker passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::current_path(launch_directory);
    std::filesystem::remove_all(root);
    return result;
}
