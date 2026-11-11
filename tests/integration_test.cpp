#include "application.h"

#include <cstring>
#include <epoxy/gl.h>
#include <stdexcept>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct ApplicationTestAccess {
    template <class Predicate> static void wait_for(Application& app, Predicate predicate)
    {
        const auto deadline = Application::Clock::now() + std::chrono::seconds(20);
        while (!predicate() && Application::Clock::now() < deadline) {
            pollfd fd{app.language_server->event_fd(), POLLIN, 0};
            poll(&fd, 1, 50);
            for (const auto& message : app.language_server->take_messages())
                app.handle_message(message);
        }
        expect(predicate(), "Timed out waiting for a language feature");
    }

    static int camera_panel(Application& app)
    {
        for (std::size_t i = 0; i < app.panels.size(); ++i)
            if (app.panels[i].path == "camera.h")
                return static_cast<int>(i);
        throw std::runtime_error("Camera source panel missing");
    }

    static Vec2 point_at(Application& app, int index, int line, std::size_t byte)
    {
        auto& panel = app.panels[index];
        reveal_line(line, panel.folds);
        app.update_mesh(panel);
        app.relayout();
        PangoRectangle glyph{};
        pango_layout_index_to_pos(panel.lines[line], static_cast<int>(byte), &glyph);
        const Vec2 world{panel.bounds.origin.x + 24 + (glyph.x + glyph.width / 2.0) / PANGO_SCALE,
                         panel.bounds.origin.y + app.header_height +
                             panel.folded.display_lines[line] * app.line_height + 11};
        app.camera.zoom = 1;
        app.camera.center = world;
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

    static void test_popup(Application& app)
    {
        app.show_popup("```cpp\nconst int value = 42;\n```", {60, 60}, true);
        app.draw();
        expect(app.popup_text.find("```") == std::string::npos && app.popup_code_lines[0],
               "Popup code fences must become highlighted code");
        auto* attributes = pango_layout_get_attributes(app.popup_layout);
        auto* iterator = pango_attr_list_get_iterator(attributes);
        const auto* weight =
            reinterpret_cast<PangoAttrInt*>(pango_attr_iterator_get(iterator, PANGO_ATTR_WEIGHT));
        expect(weight && weight->value == PANGO_WEIGHT_BOLD,
               "Popup keywords must retain bold syntax styling");
        pango_attr_iterator_destroy(iterator);
        XEvent motion{};
        motion.type = MotionNotify;
        motion.xmotion.x = static_cast<int>(app.popup_bounds.origin.x + 20);
        motion.xmotion.y = static_cast<int>(app.popup_bounds.origin.y + 20);
        app.handle_motion(motion);
        app.update_timers(Application::Clock::now() + std::chrono::seconds(1));
        expect(!app.popup_text.empty(), "Popup must stay open while the pointer is inside");
        app.handle_button(click(app.mouse_position));
        app.handle_release(click(app.mouse_position));
        expect(!app.popup_text.empty(),
               "Clicking popup code must not dismiss it or pan the canvas");
        motion.xmotion.x = -10;
        motion.xmotion.y = -10;
        app.handle_motion(motion);
        app.update_timers(Application::Clock::now() + std::chrono::milliseconds(100));
        expect(!app.popup_text.empty(),
               "Crossing the popup gap must allow a dismissal grace period");
        app.update_timers(Application::Clock::now() + std::chrono::seconds(1));
        expect(app.popup_text.empty(), "Popup must close after leaving it");
    }

    static void test_folding(Application& app, int index)
    {
        auto& panel = app.panels[index];
        expect(!panel.folds.empty(), "Clangd must provide foldable code blocks");
        auto& block = panel.folds.back();
        block.collapsed = true;
        const int hidden = block.first + 1;
        app.update_mesh(panel);
        expect(panel.folded.display_lines[hidden] == panel.folded.display_lines[block.first],
               "Collapsed bodies must map to their visible header");
        const lsp::Json location{{"uri", panel.uri},
                                 {"range", {{"start", {{"line", hidden}, {"character", 0}}}}}};
        expect(app.jump_to(location), "Jumping into a collapsed block must succeed");
        expect(panel.folded.source_lines[panel.folded.display_lines[hidden]] == hidden,
               "Navigation must reveal a hidden target and all ancestors");
        expect(app.go_back(), "Navigation must preserve a return location after graph relayout");
    }

    static void test_symbols(Application& app, int index)
    {
        auto& panel = app.panels[index];
        auto camera = std::find_if(panel.symbols.begin(), panel.symbols.end(),
                                   [](const auto& node) { return node.name == "Camera"; });
        expect(camera != panel.symbols.end() && !camera->children.empty(),
               "Class symbols must have nested fields and methods");
        app.hide_popup();
        app.popup_origin = {30, 30};
        app.show_outline(index);
        app.draw();
        const auto byte = app.popup_text.find("Camera");
        PangoRectangle rectangle{};
        pango_layout_index_to_pos(app.popup_layout, static_cast<int>(byte), &rectangle);
        const Vec2 arrow{app.popup_bounds.origin.x + 17,
                         app.popup_bounds.origin.y + 12 + rectangle.y / PANGO_SCALE + 8};
        app.handle_button(click(arrow));
        app.handle_release(click(arrow));
        expect(camera->expanded && app.popup_text.find("zoom_at") != std::string::npos,
               "Clicking an outline chevron must expand the actual nested symbol hierarchy");
    }

    static void press_and_release(Application& app, Vec2 point)
    {
        auto event = click(point, 0);
        app.handle_button(event);
        event.type = ButtonRelease;
        event.xbutton.state = Button1Mask;
        app.handle_event(event);
    }

    static void test_context_navigation(Application& app, int index)
    {
        auto& panel = app.panels[index];
        const auto camera = std::find_if(panel.symbols.begin(), panel.symbols.end(),
                                         [](const auto& node) { return node.name == "Camera"; });
        const auto byte = lsp::utf16_to_byte(panel.text[camera->line], camera->column);
        app.hide_popup();
        const auto point = point_at(app, index, camera->line, byte);
        expect(app.definition_at_pointer(),
               "A class definition must be recognized from semantic metadata");
        press_and_release(app, point);
        expect(app.connection_subject.active && app.connection_subject.name == "Camera" &&
                   app.popup_text.empty(),
               "A click on a symbol must make it the subject of arrows, not open a list");
        wait_for(app, [&]() {
            return !app.connection_requests.second && !app.connection_links.empty();
        });
        expect(std::any_of(app.connection_links.begin(), app.connection_links.end(),
                           [](const auto& link) { return !link.defines; }),
               "A definition's arrows must lead to the places that use it");
        int usage = -1;
        for (std::size_t i = 0; i < panel.text.size(); ++i)
            if (panel.text[i].find("const Camera&") != std::string::npos) {
                usage = static_cast<int>(i);
                break;
            }
        expect(usage >= 0, "Expected a real Camera type usage");
        const auto use_byte = panel.text[usage].find("Camera");
        const auto use_point = point_at(app, index, usage, use_byte);
        expect(!app.definition_at_pointer(),
               "A symbol reference must not be classified as its definition");
        press_and_release(app, use_point);
        wait_for(app, [&]() {
            return std::any_of(
                app.connection_links.begin(), app.connection_links.end(),
                [&](const auto& link) { return link.defines && link.at.line == camera->line; });
        });
        app.draw();
        expect(!app.connection_words.empty() || !app.connection_markers.empty(),
               "A use's arrows must lead to its definition: a highlighted word or a marker");
        app.clear_connections();
    }

    static void test_inline_toggles_and_cursor(Application& app, int index)
    {
        app.hide_popup();
        auto& panel = app.panels[index];
        auto& block = panel.folds.back();
        reveal_line(block.first, panel.folds);
        block.collapsed = false;
        app.update_mesh(panel);
        app.relayout();
        const auto bounds = app.fold_toggle_bounds(panel, block.first);
        app.camera.zoom = 1;
        app.camera.center = {bounds.origin.x - 20, bounds.origin.y + app.line_height / 2.0};
        app.zoom_animation.cancel(app.camera);
        const auto toggle =
            app.camera.world_to_screen({bounds.origin.x + 6, bounds.origin.y + 10}, app.viewport());
        expect(app.fold_at(toggle).line == block.first,
               "A fold control must live beside its block header");
        const auto gutter = app.camera.world_to_screen(
            {panel.bounds.origin.x + 8, bounds.origin.y + 10}, app.viewport());
        expect(app.fold_at(gutter).panel < 0 && app.fold_at({-1, -1}).panel < 0,
               "Fold controls must stay hidden in the gutter and away from their code");
        app.handle_button(click(toggle));
        expect(block.collapsed && !app.drag_button,
               "Clicking an inline chevron must fold without panning");
        app.mouse_position = {-1, -1};
        app.update_cursor();
        expect(app.active_cursor == app.normal_cursor,
               "Blank space must use a normal arrow cursor");
        const auto first = panel.text[block.first].find_first_not_of(" \t");
        const auto text_point = point_at(app, index, block.first, first);
        app.update_cursor();
        expect(app.active_cursor == app.text_cursor, "Source text must use a text cursor");
        auto pan = click(text_point);
        pan.xbutton.button = Button2;
        app.handle_button(pan);
        expect(app.drag_button == Button2 && app.active_cursor == app.drag_cursor,
               "The pan cursor must appear only during an active pan");
        app.handle_release(pan);
        expect(!app.drag_button && app.active_cursor == app.text_cursor,
               "Releasing a pan must immediately restore the text cursor beneath it");
    }

    static void capture_popup(Application& app)
    {
        const auto* output = std::getenv("SOURCE_CANVAS_POPUP_SCREENSHOT");
        if (!output)
            return;
        const auto camera = app.camera;
        app.camera.center = {-10000, -10000};
        app.update_timers(Application::Clock::now());
        app.renderer->begin(app.width, app.height);
        app.draw_popup();
        app.renderer->finish();
        std::vector<unsigned char> pixels(static_cast<std::size_t>(app.width) * app.height * 4);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, app.width, app.height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
        glReadBuffer(GL_BACK);
        auto* image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, app.width, app.height);
        auto* bytes = cairo_image_surface_get_data(image);
        const auto stride = cairo_image_surface_get_stride(image);
        for (int y = 0; y < app.height; ++y)
            std::memcpy(bytes + y * stride, pixels.data() + (app.height - y - 1) * app.width * 4,
                        static_cast<std::size_t>(app.width) * 4);
        cairo_surface_mark_dirty(image);
        cairo_surface_write_to_png(image, output);
        cairo_surface_destroy(image);
        app.camera = camera;
    }
    static void test_compact_documentation(Application& app)
    {
        const std::string source =
            "\n\n<h3>Camera</h3>\n\n<p>A compact explanation with **emphasis** and "
            "`inline_code`.</p>\n\n\n<p>A second paragraph.</p>\n<pre><code>const int first = "
            "1;\n\nreturn first;\n</code></pre>";
        app.show_popup(source, {60, 60}, true);
        app.draw();
        DocumentView reading;
        app.update_document(reading, source, app.cached_popup_width);
        int popup_height = 0;
        pango_layout_get_pixel_size(app.popup_layout, nullptr, &popup_height);
        expect(popup_height < reading.size.y * 0.8,
               "Popup paragraphs and HTML-generated blank lines must use a compact reading rhythm");
        expect(app.popup_text.find("</h3>") == std::string::npos,
               "HTML heading tags must not leak into the popup");
        expect(app.popup_text == reading.document.text &&
                   app.popup_text.find("const int first = 1;\n\nreturn first;") !=
                       std::string::npos,
               "Compact typography must preserve code whitespace and markup byte ranges");
        const auto literal = app.popup_text.find("return first;");
        PangoRectangle bounds{};
        pango_layout_index_to_pos(app.popup_layout, static_cast<int>(literal), &bounds);
        expect(bounds.height >= 14 * PANGO_SCALE, "Compact code must retain a legible line height");
        expect(app.popup_bounds.size.y < popup_height + 24,
               "Popup bounds must exclude trailing blank markup rows");
        capture_popup(app);
        app.renderer->release(reading.mesh);
        app.hide_popup();
    }
    static void test_typeset_documentation(Application& app)
    {
        app.show_popup("## Camera\nReadable **prose** with <code>const int n = 2;</code>.\n"
                       "The transform is $s = v + z(w-c)$.\n\\[\\frac{1}{z} + \\sqrt{x}\\]",
                       {60, 60}, true);
        app.draw();
        expect(app.popup_prose && app.popup_styles.size() == 2 && app.popup_math.size() == 2,
               "Documentation must combine prose, code typography and equations");
        expect(std::string(pango_font_description_get_family(
                   pango_layout_get_font_description(app.popup_layout))) == "Noto Serif",
               "Documentation prose must use the bundled serif reading face");
        const auto code = app.popup_text.find("const int");
        auto* iterator =
            pango_attr_list_get_iterator(pango_layout_get_attributes(app.popup_layout));
        bool monospace = false;
        do {
            int begin = 0, end = 0;
            pango_attr_iterator_range(iterator, &begin, &end);
            if (code >= static_cast<std::size_t>(begin) && code < static_cast<std::size_t>(end)) {
                const auto* font = reinterpret_cast<PangoAttrFontDesc*>(
                    pango_attr_iterator_get(iterator, PANGO_ATTR_FONT_DESC));
                monospace = font && std::string(pango_font_description_get_family(font->desc)) ==
                                        "JetBrains Mono";
            }
        } while (pango_attr_iterator_next(iterator));
        pango_attr_iterator_destroy(iterator);
        expect(monospace, "Inline code must retain JetBrains Mono inside serif prose");
        const auto deadline = Application::Clock::now() + std::chrono::seconds(12);
        auto ready = [&]() {
            return std::all_of(app.popup_math.begin(), app.popup_math.end(), [&](const auto& math) {
                const auto found =
                    app.math_artwork.find(MathTypesetter::key(math.expression, math.display));
                return found != app.math_artwork.end() && found->second.ready;
            });
        };
        while (!ready() && Application::Clock::now() < deadline) {
            pollfd fd{app.math_typesetter->event_fd(), POLLIN, 0};
            poll(&fd, 1, 50);
            app.poll_math();
        }
        expect(ready(), "LaTeX equations must reach the actual GPU popup as cached artwork");
        app.draw();
        expect(app.popup_mesh.vertices > 0,
               "Typeset documentation must upload a renderable GPU mesh");
        app.hide_popup();
    }

    static void test_live_documentation(Application& app, int index)
    {
        auto& panel = app.panels[index];
        const auto symbol = std::find_if(panel.symbols.begin(), panel.symbols.end(),
                                         [](const auto& node) { return node.name == "Camera"; });
        const auto byte = lsp::utf16_to_byte(panel.text[symbol->line], symbol->column);
        point_at(app, index, symbol->line, byte);
        app.hover_request = app.query_at_pointer("textDocument/hover");
        wait_for(app, [&]() { return !app.popup_text.empty(); });
        expect(app.popup_prose && app.popup_text.find("world coordinates") != std::string::npos,
               "Actual clangd documentation must reach the prose renderer");
        expect(app.popup_math.size() == 4 && !app.popup_inline_code.empty() &&
                   app.popup_text.find("\\*") == std::string::npos,
               "Real source documentation must recover math, examples and HTML through clangd");
        app.draw();
        auto ready = [&]() {
            return std::all_of(app.popup_math.begin(), app.popup_math.end(), [&](const auto& math) {
                const auto found =
                    app.math_artwork.find(MathTypesetter::key(math.expression, math.display));
                return found != app.math_artwork.end() && found->second.ready;
            });
        };
        const auto deadline = Application::Clock::now() + std::chrono::seconds(12);
        while (!ready() && Application::Clock::now() < deadline) {
            pollfd fd{app.math_typesetter->event_fd(), POLLIN, 0};
            poll(&fd, 1, 50);
            app.poll_math();
        }
        expect(ready(),
               "Every equation from actual clangd documentation must typeset successfully");
        app.hide_popup();
        std::string long_doc = "## A long explanation\n";
        for (int i = 0; i < 80; ++i)
            long_doc += "Readable documentation paragraph " + std::to_string(i) + ".\n";
        app.show_popup(long_doc, {30, 30}, true);
        app.draw();
        const auto zoom = app.camera.zoom;
        auto wheel = click({app.popup_bounds.origin.x + 20, app.popup_bounds.origin.y + 40});
        wheel.xbutton.button = Button5;
        app.handle_button(wheel);
        app.draw();
        expect(app.popup_scroll > 0 && app.camera.zoom == zoom &&
                   app.popup_bounds.size.y <= app.height,
               "Long prose must scroll within its viewport without zooming the canvas");
        app.hide_popup();
    }

    static void run(Application& app)
    {
        expect(app.initialize(), "Application must initialize");
        app.initial_view = false;
        app.camera.fit(app.content_size, app.viewport());
        const int index = camera_panel(app);
        XWindowAttributes window{};
        const auto mapped_deadline = Application::Clock::now() + std::chrono::seconds(2);
        while (Application::Clock::now() < mapped_deadline) {
            XGetWindowAttributes(app.display, app.window, &window);
            if (window.map_state == IsViewable)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        expect(window.map_state == IsViewable,
               "The test window must be mapped before pointer events");
        wait_for(app, [&]() {
            return !app.panels[index].symbols.empty() && !app.panels[index].semantics.empty();
        });
        test_inline_toggles_and_cursor(app, index);
        test_compact_documentation(app);
        test_typeset_documentation(app);
        test_live_documentation(app, index);
        test_popup(app);
        test_folding(app, index);
        test_symbols(app, index);
        test_context_navigation(app, index);
        std::cout
            << "Connections, popup, folding, symbol hierarchy and navigation checks passed.\n";
    }
};

int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    try {
        Application app;
        ApplicationTestAccess::run(app);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
