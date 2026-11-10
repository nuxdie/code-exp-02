#include "application.h"
#include <X11/Xatom.h>
#include <unistd.h>

namespace {
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
struct ApplicationTestAccess {
    static void key(Application& app, KeySym symbol, unsigned modifiers = 0)
    {
        XEvent event{};
        event.type = KeyPress;
        event.xkey.display = app.display;
        event.xkey.window = app.window;
        event.xkey.keycode = XKeysymToKeycode(app.display, symbol);
        event.xkey.state = modifiers;
        app.handle_key(event);
    }
    static void query(Application& app, const std::string& text)
    {
        app.search.query.set(text);
        app.update_search();
        app.draw();
    }
    static void test_matching(Application& app)
    {
        key(app, XK_slash);
        expect(app.search.active && app.search.query.text.empty(),
               "Slash must open an empty search without becoming part of the query");
        query(app, "needle");
        expect(app.search.matches.size() == 4,
               "Global search must include file names and every occurrence, ignoring case");
        expect(app.search.matches.front().filename,
               "Matching file names must precede content hits");
        expect(std::count_if(app.search.matches.begin(), app.search.matches.end(),
                             [](const auto& match) { return match.filename; }) == 1,
               "A matching file name must be distinguished from content hits");
        query(app, "CAFÉ");
        expect(app.search.matches.size() == 1 &&
                   app.search.matches.front().preview.find("café") != std::string::npos,
               "Unicode case-insensitive search must retain the original source text");
        query(app, "[literal]");
        expect(app.search.matches.size() == 1,
               "Search punctuation must be literal rather than interpreted as a regex");
        query(app, "absent phrase");
        expect(app.search.matches.empty(), "An unmatched query must have no stale results");
    }
    static void test_navigation(Application& app)
    {
        query(app, "🌍");
        const auto byte = app.panels.front().text.front().find("🌍");
        key(app, XK_Return);
        expect(!app.search.active && app.focused_line && app.focused_line->byte == byte,
               "Enter must jump to the actual UTF-8 byte through the LSP UTF-16 location format");
        for (auto& fold : app.panels.front().folds)
            fold.collapsed = true;
        app.update_mesh(app.panels.front());
        app.open_search();
        query(app, "return 42");
        key(app, XK_Return);
        expect(app.focused_line && app.focused_line->line == 3 &&
                   std::none_of(app.panels.front().folds.begin(), app.panels.front().folds.end(),
                                [](const auto& fold) { return fold.collapsed; }),
               "Jumping to a hidden match must reveal its enclosing blocks");
        app.open_search();
        query(app, "needle.h");
        expect(app.search.hits.size() == 1, "A file-name search must provide a clickable result");
        const auto hit = app.search.hits.front().first;
        XEvent click{};
        click.type = ButtonPress;
        click.xbutton.button = Button1;
        click.xbutton.x = static_cast<int>(hit.origin.x + 8);
        click.xbutton.y = static_cast<int>(hit.origin.y + 8);
        app.handle_button(click);
        expect(!app.search.active && app.focused_line &&
                   app.panels[app.focused_line->panel].path == "needle.h",
               "Clicking a result must navigate to its file on the canvas");
        app.open_search();
        query(app, "needle");
        key(app, XK_Down);
        expect(app.search.selected == 1, "Arrow keys must navigate results instead of panning");
        key(app, XK_Escape);
    }
    static void test_live_sources(Application& app, const std::filesystem::path& root)
    {
        app.open_search();
        query(app, "return 42");
        expect(app.search.matches.size() == 1, "The live fixture must initially match");
        std::ofstream(root / "main.cpp") << "int replacement() { return 73; }\n";
        const auto deadline = Application::Clock::now() + std::chrono::seconds(4);
        while (!app.search.matches.empty() && Application::Clock::now() < deadline) {
            app.poll_project();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        expect(app.search.active && app.search.matches.empty(),
               "Disk updates must refresh results without closing the active search");
        query(app, "return 73");
        app.chat->disabled = false;
        app.save_session();
        app.search.active = false;
        app.search.query.set("");
        app.load_session();
        app.chat->disabled = true;
        expect(app.search.active && app.search.query.text == "return 73" &&
                   app.search.matches.size() == 1,
               "Viewer reload must restore the open search and its live results");
        app.close_search();
    }
    static void test_shortcuts(Application& app)
    {
        app.chat->focused = true;
        app.chat->input.set("");
        key(app, XK_slash);
        key(app, XK_slash, ShiftMask);
        expect(app.chat->input.text == "/?" && !app.search.active && !app.shortcuts_visible,
               "Slash and question mark must remain ordinary text in the composer");
        key(app, XK_Escape);
        key(app, XK_slash, ShiftMask);
        expect(app.shortcuts_visible && app.popup_text.find("Ctrl+Shift+A") != std::string::npos &&
                   app.popup_text.find("Shift+F12") != std::string::npos &&
                   app.popup_text.find("Ctrl+Space") != std::string::npos &&
                   app.popup_text.find("Ctrl+V") != std::string::npos,
               "Question mark must open the complete shortcut guide");
        app.draw();
        expect(std::string(pango_font_description_get_family(
                   pango_layout_get_font_description(app.popup_layout))) == "sans-serif",
               "The shortcut guide must use UI typography rather than documentation prose");
        const int original_width = app.width;
        app.width = 360;
        app.draw();
        expect(app.popup_bounds.origin.x >= 0 &&
                   app.popup_bounds.origin.x + app.popup_bounds.size.x <= app.width,
               "The shortcut guide must remain inside narrow windows");
        app.width = original_width;
        app.draw();
        XEvent motion{};
        motion.type = MotionNotify;
        motion.xmotion.x = 0;
        motion.xmotion.y = app.height - 1;
        app.handle_motion(motion);
        expect(app.popup_dismiss_due == Application::Clock::time_point::max(),
               "The shortcut guide must stay open while moving the pointer away");
        XEvent leave{};
        leave.type = LeaveNotify;
        app.handle_event(leave);
        expect(app.popup_dismiss_due == Application::Clock::time_point::max(),
               "Leaving the application window must keep the shortcut guide open");
        key(app, XK_Page_Down);
        expect(app.popup_scroll > 0, "The complete guide must support keyboard scrolling");
        key(app, XK_Escape);
        expect(app.popup_text.empty(), "Escape must close the guide");
        key(app, XK_F1);
        expect(app.shortcuts_visible, "F1 must remain an alias for the same guide");
        key(app, XK_slash);
        expect(app.search.active && !app.shortcuts_visible,
               "Slash must open search from the guide");
        app.close_search();
    }
    static void test_editing(Application& app)
    {
        app.open_search();
        query(app, "café");
        key(app, XK_a, ControlMask);
        key(app, XK_c, ControlMask);
        expect(app.clipboard_text == "café", "Search selection must copy exact UTF-8 text");
        key(app, XK_x, ControlMask);
        expect(app.search.query.text.empty(), "Cut must edit the query and refresh results");
        const auto property = XInternAtom(app.display, "SOURCE_CANVAS_SEARCH_PASTE", False);
        const auto utf8 = XInternAtom(app.display, "UTF8_STRING", False);
        const std::string text = "replacement";
        XChangeProperty(app.display, app.window, property, utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(text.data()),
                        static_cast<int>(text.size()));
        XEvent paste{};
        paste.type = SelectionNotify;
        paste.xselection.property = property;
        app.handle_event(paste);
        expect(app.search.query.text == text && app.search.matches.size() == 1,
               "Clipboard notifications must paste into search and update matches");
        app.close_search();
        const auto draft = app.chat->input.text;
        XChangeProperty(app.display, app.window, property, utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(text.data()),
                        static_cast<int>(text.size()));
        app.handle_event(paste);
        expect(app.chat->input.text == draft,
               "A late search paste must never be inserted into an agent draft");
    }
    static void test_options(Application& app)
    {
        app.hide_popup();
        app.text_selection = {};
        app.chat->focused = true;
        key(app, XK_Escape);
        expect(!app.chat->focused && app.options_page == Application::OptionsPage::Closed,
               "Escape in an editor must only leave the editor");
        app.show_popup("Documentation", {20, 20});
        key(app, XK_Escape);
        expect(app.popup_text.empty() && app.options_page == Application::OptionsPage::Closed,
               "Escape must dismiss documentation before opening options");
        key(app, XK_Escape);
        expect(app.options_page == Application::OptionsPage::Options,
               "Escape on the unfocused canvas must open options");
        key(app, XK_Return);
        expect(app.options_page == Application::OptionsPage::Settings,
               "Options must offer a working Settings entry");
        key(app, XK_3);
        expect(app.appearance == "dark", "Settings must allow a dark appearance override");
        key(app, XK_2);
        expect(app.appearance == "light", "Settings must allow a light appearance override");
        key(app, XK_1);
        expect(app.appearance == "system", "Settings must allow following the desktop again");
        app.draw();
        XEvent click{};
        click.xbutton.button = Button1;
        PangoRectangle row{};
        pango_layout_index_to_pos(app.popup_layout, static_cast<int>(app.popup_text.find("Dark")),
                                  &row);
        const Vec2 point{app.popup_bounds.origin.x + 30,
                         app.popup_bounds.origin.y + 12 + (row.y + row.height / 2.0) / PANGO_SCALE};
        app.popup_button(click, point, true);
        expect(app.appearance == "dark", "Settings rows must also respond to mouse clicks");
        key(app, XK_Escape);
        expect(app.options_page == Application::OptionsPage::Options,
               "Escape from Settings must return to Options");
        key(app, XK_Escape);
        expect(app.options_page == Application::OptionsPage::Closed,
               "Escape from Options must close the menu");
        app.chat->disabled = false;
        app.save_session();
        app.appearance = "system";
        app.load_session();
        app.chat->disabled = true;
        expect(app.appearance == "dark", "Appearance must survive a session reload");
    }
    static void run(Application& app, const std::filesystem::path& root)
    {
        expect(app.initialize(), "The global-search canvas must initialize");
        app.chat->focused = false;
        test_matching(app);
        test_navigation(app);
        test_live_sources(app, root);
        test_shortcuts(app);
        test_editing(app);
        test_options(app);
    }
};
int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto root =
        std::filesystem::temp_directory_path() / ("canvas-search-" + std::to_string(getpid()));
    int result = 0;
    try {
        std::filesystem::create_directories(root);
        std::ofstream(root / "main.cpp")
            << "const char* text = \"Needle needle café 🌍 [literal]\";\n"
               "int target() {\n // needle\n return 42;\n}\n";
        std::ofstream(root / "needle.h") << "int helper;\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", root.c_str(), 1);
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
        setenv("SOURCE_CANVAS_AUTO_BUILD", "0", 1);
        setenv("SOURCE_CANVAS_STATE_FILE", (root / "session.json").c_str(), 1);
        unsetenv("SOURCE_CANVAS_DISABLE_SESSION");
        Application app;
        ApplicationTestAccess::run(app, root);
        std::cout << "Global search, Unicode navigation, live results, reload and shortcut guide "
                     "passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}
