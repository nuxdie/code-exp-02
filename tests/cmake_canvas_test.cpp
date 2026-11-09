#include "application.h"
#include <unistd.h>

namespace {
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
struct ApplicationTestAccess {
    template <class Predicate> static void wait_for(Application& app, Predicate predicate)
    {
        const auto deadline = Application::Clock::now() + std::chrono::seconds(12);
        while (!predicate() && Application::Clock::now() < deadline) {
            app.poll_project();
            for (auto* server : {app.language_server.get(), app.cmake_server.get()})
                if (server)
                    for (const auto& message : server->take_messages())
                        app.handle_message(message);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!predicate()) {
            std::cerr << "CMake ready=" << app.cmake_ready << " failure=" << app.cmake_failure
                      << '\n';
            for (const auto& source : app.panels)
                if (source.cmake)
                    std::cerr << source.path << " links=" << source.links.dump() << '\n';
        }
        expect(predicate(), "Timed out waiting for CMake language features");
    }
    static SourcePanel& panel(Application& app, const std::string& path)
    {
        const auto found = std::find_if(app.panels.begin(), app.panels.end(),
                                        [&](const auto& entry) { return entry.path == path; });
        expect(found != app.panels.end(), "Fixture source card must exist");
        return *found;
    }
    static Vec2 point_at(Application& app, const std::string& path, int line, std::size_t byte)
    {
        auto& source = panel(app, path);
        reveal_line(line, source.folds);
        app.update_mesh(source);
        PangoRectangle glyph{};
        pango_layout_index_to_pos(source.lines[line], static_cast<int>(byte), &glyph);
        app.camera.zoom = 0.8;
        app.camera.center = {source.bounds.origin.x + 24 +
                                 static_cast<double>(glyph.x + glyph.width / 2) / PANGO_SCALE,
                             source.bounds.origin.y + app.header_height +
                                 source.folded.display_lines[line] * app.line_height + 8};
        app.zoom_animation.cancel(app.camera);
        app.mouse_position = app.viewport().center();
        return app.mouse_position;
    }
    static XEvent click(Vec2 point, unsigned state = 0)
    {
        XEvent event{};
        event.type = ButtonPress;
        event.xbutton.button = Button1;
        event.xbutton.state = state;
        event.xbutton.x = static_cast<int>(point.x);
        event.xbutton.y = static_cast<int>(point.y);
        return event;
    }
    static bool links_to(Application& app, const std::string& target)
    {
        const auto uri = panel(app, target).uri;
        const auto& links = panel(app, "CMakeLists.txt").links;
        return std::any_of(links.begin(), links.end(),
                           [&](const auto& link) { return link.value("target", "") == uri; });
    }
    static void test_file_links(Application& app)
    {
        wait_for(app, [&] { return app.cmake_ready && links_to(app, "main.cpp"); });
        expect(panel(app, "cmake/Helpers.cmake").cmake,
               "CMake module cards must use the CMake server too");
        const auto lines = panel(app, "CMakeLists.txt").text;
        auto point = point_at(app, "CMakeLists.txt", 2, lines[2].find("main.cpp") + 2);
        app.handle_button(click(point));
        app.handle_release(click(point));
        expect(app.focused_line && app.panels[app.focused_line->panel].path == "main.cpp" &&
                   !app.drag_button,
               "Clicking a server-provided CMake filename link must open its card without panning");
        expect(app.go_back(), "File links must preserve navigation history");
        point = point_at(app, "CMakeLists.txt", 2, lines[2].find("space file.cpp") + 2);
        app.handle_button(click(point, ControlMask));
        expect(app.focused_line && app.panels[app.focused_line->panel].path == "space file.cpp",
               "Ctrl+click must also open quoted filenames containing spaces");
    }
    static void test_hover_and_definition(Application& app)
    {
        point_at(app, "CMakeLists.txt", 0, 7);
        app.hover_request = app.query_at_pointer("textDocument/hover");
        wait_for(app, [&] { return !app.popup_text.empty(); });
        expect(app.popup_text.find("minimum") != std::string::npos,
               "CMake hover must use real command documentation");
        app.hide_popup();
        point_at(app, "CMakeLists.txt", 5, 12);
        app.navigation_request = app.query_at_pointer("textDocument/definition");
        wait_for(app, [&] {
            return app.focused_line &&
                   app.panels[app.focused_line->panel].path == "CMakeLists.txt" &&
                   app.focused_line->line == 4;
        });
    }
    static void test_live_links(Application& app, const std::filesystem::path& root)
    {
        const auto old_links = panel(app, "CMakeLists.txt").links;
        auto updated = panel(app, "CMakeLists.txt").source;
        updated.replace(updated.find("main.cpp"), std::string("main.cpp").size(),
                        "replacement.cpp");
        std::ofstream(root / "CMakeLists.txt") << updated;
        wait_for(app,
                 [&] { return links_to(app, "replacement.cpp") && !links_to(app, "main.cpp"); });
        const auto uri = panel(app, "CMakeLists.txt").uri;
        app.handle_message({"textDocument/documentLink", uri, 999, old_links, {}, 1, "cmake"});
        expect(!links_to(app, "main.cpp"),
               "Stale links must not replace the current CMake source version");
    }
    static void run(Application& app, const std::filesystem::path& root)
    {
        expect(app.initialize(), "CMake canvas must initialize");
        app.initial_view = false;
        const auto deadline = Application::Clock::now() + std::chrono::seconds(2);
        XWindowAttributes attributes{};
        do {
            XGetWindowAttributes(app.display, app.window, &attributes);
            if (attributes.map_state == IsViewable)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (Application::Clock::now() < deadline);
        expect(attributes.map_state == IsViewable, "CMake test window must be mapped");
        test_file_links(app);
        test_hover_and_definition(app);
        test_live_links(app, root);
        app.draw();
    }
};
int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto root =
        std::filesystem::temp_directory_path() / ("cmake-canvas-" + std::to_string(getpid()));
    int result = 0;
    try {
        std::filesystem::create_directories(root / "cmake");
        std::ofstream(root / "CMakeLists.txt")
            << "cmake_minimum_required(VERSION 3.16)\nproject(Fixture LANGUAGES "
               "NONE)\nadd_library(example main.cpp \"space "
               "file.cpp\")\ninclude(cmake/Helpers.cmake)\nset(ANSWER "
               "42)\nmessage(\"${ANSWER}\")\n";
        for (const auto* name : {"main.cpp", "space file.cpp", "replacement.cpp"})
            std::ofstream(root / name) << "int example;\n";
        std::ofstream(root / "cmake/Helpers.cmake") << "function(helper)\nendfunction()\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", root.c_str(), 1);
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_SESSION", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
        setenv("SOURCE_CANVAS_AUTO_BUILD", "0", 1);
        Application app;
        ApplicationTestAccess::run(app, root);
        std::cout << "Live CMake links, hover, definitions and source updates passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}
