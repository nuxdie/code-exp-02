#include "application.h"
#include <epoxy/gl.h>
#include <string_view>
#include <unistd.h>

namespace {
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
struct ApplicationTestAccess {
    static Vec2 source_point(Application& app)
    {
        auto& panel = app.panels.front();
        PangoRectangle glyph{};
        pango_layout_index_to_pos(panel.lines[1].get(), 5, &glyph);
        app.initial_view = false;
        app.camera.zoom = 1;
        app.camera.center = {panel.bounds.origin.x + 24 +
                                 static_cast<double>(glyph.x + glyph.width / 2) / PANGO_SCALE,
                             panel.bounds.origin.y + app.header_height + app.line_height + 10};
        app.zoom_animation.cancel(app.camera);
        app.mouse_position = app.viewport().center();
        return app.mouse_position;
    }
    static void await_symbols(Application& app)
    {
        const auto deadline = Application::Clock::now() + 8s;
        while (app.panels.front().symbols.empty() && Application::Clock::now() < deadline) {
            for (const auto& message : app.language_server->take_messages())
                app.handle_message(message);
            std::this_thread::sleep_for(5ms);
        }
        expect(!app.panels.front().symbols.empty(),
               "The context fixture must have real language-server symbols");
    }
    static void test_spawn_and_context(Application& app)
    {
        await_symbols(app);
        const auto pointer = source_point(app);
        auto context = app.agent_context(pointer);
        expect(context["path"] == "main.cpp" && context["line"] == 2 &&
                   context["symbol"] == "Camera::value",
               ("Spawn context must capture the pointed file, line and enclosing symbol: " +
                context.dump())
                   .c_str());
        app.text_selection.surface = TextSurface::Source;
        app.text_selection.key = app.panels.front().path;
        app.text_selection.range.set(app.panels.front().source);
        const auto begin = app.panels.front().source.find("value");
        app.text_selection.range.place(begin);
        app.text_selection.range.place(begin + 5, true);
        context = app.agent_context({0, 0});
        expect(context["selected_text"] == "value" && context["line"] == 2 &&
                   context["path"] == "main.cpp",
               "Selected source must retain its file and line when spawning on blank canvas");
        app.chat->conversation.thread_id = "original-thread";
        app.chat->input.set("Original draft λ");
        auto* const original = app.chat.get();
        const Vec2 location{-140, 260};
        const auto camera = app.camera;
        app.spawn_agent_window(location, context);
        expect(app.chat.get() != original && app.chat_windows.size() == 1 &&
                   app.chat->bounds.origin.x == location.x &&
                   app.chat->bounds.origin.y == location.y,
               "The new agent must be placed exactly at the requested world position");
        expect(app.camera.center.x == camera.center.x && app.camera.center.y == camera.center.y &&
                   app.camera.zoom == camera.zoom,
               "Spawning at the pointer must preserve the user's camera position and zoom");
        expect(app.chat->conversation.fresh && app.chat->conversation.thread_id.empty() &&
                   app.chat_windows.front()->input.text == "Original draft λ",
               "Spawning must preserve the old card and give the new card an independent draft and "
               "thread");
        expect(
            app.chat->conversation.canvas_context.contains("Camera") &&
                app.chat->conversation.entries.front().role == "Context",
            "Context must be visible locally and attached without manufacturing a human message");
        app.chat->input.set("New card draft");
        app.draw();
        expect(app.chat->context_preview.document.text.starts_with("Context included") &&
                   app.chat->context_preview.document.text.contains("main.cpp:2") &&
                   app.chat->context_preview.document.text.contains("Camera::value") &&
                   !app.chat->context_preview.document.text.contains("Selected text:") &&
                   app.prompt_source_range(app.panels.front()) == std::pair{begin, begin + 5} &&
                   app.text_selection.surface == TextSurface::Inactive &&
                   app.chat->editor.origin.y > app.chat->bounds.origin.y &&
                   app.chat->editor.size.y == 90,
               "An unsent prompt must mark the source independently of the live text selection");
        const auto old_point = app.camera.world_to_screen(
            {original->bounds.origin.x + 12, original->bounds.origin.y + 12}, app.viewport());
        expect(
            app.activate_chat(old_point) && app.chat.get() == original,
            "Clicking an existing agent must activate that card rather than route to the new one");
        expect(app.chat->input.text == "Original draft λ" &&
                   app.chat_windows.front()->input.text == "New card draft",
               "Activation must not mix independent drafts");
    }
    static void test_reload_state(Application& app)
    {
        app.chat->conversation.title = "Original Codex title λ";
        app.chat_windows.front()->conversation.title = "Second Codex title";
        app.chat->disabled = false;
        app.save_session();
        app.chat->input.set("");
        app.chat_windows.clear();
        app.load_session();
        app.draw();
        expect(app.chat->input.text == "Original draft λ" && app.chat_windows.size() == 1 &&
                   app.chat->conversation.title == "Original Codex title λ" &&
                   app.chat_windows.front()->conversation.title == "Second Codex title" &&
                   app.chat_windows.front()->input.text == "New card draft" &&
                   app.chat_windows.front()->conversation.fresh &&
                   app.chat_windows.front()->composing &&
                   app.chat_windows.front()->bounds.size.y > 90 &&
                   app.chat_windows.front()->context_preview.document.text.contains("main.cpp:2") &&
                   app.chat_windows.front()->context["path"] == "main.cpp",
               "Hot reload must preserve every card, independent draft and source context");
        app.chat->disabled = true;
        app.chat_windows.front()->conversation.questions.push_back(
            {1, "approval", "Approval", lsp::Json::object()});
        expect(app.agents_pending(), "Pending requests in an inactive card must defer reload");
        app.chat_windows.front()->conversation.questions.clear();
        expect(!app.agents_pending(), "Reload must become available once all cards are settled");
    }
    static void test_input_and_close(Application& app)
    {
        app.chat->visible = false;
        for (auto& card : app.chat_windows)
            card->visible = false;
        const auto pointer = source_point(app);
        const auto world = app.camera.screen_to_world(pointer, app.viewport());
        XEvent click{};
        click.type = ButtonPress;
        click.xbutton.button = Button3;
        click.xbutton.x = static_cast<int>(pointer.x);
        click.xbutton.y = static_cast<int>(pointer.y);
        app.handle_button(click);
        app.draw();
        expect(app.chat_windows.size() == 2 && app.chat->visible && app.chat->composing &&
                   app.chat->focused && app.chat->bounds.origin.x == world.x &&
                   app.chat->bounds.origin.y > world.y && app.popup_text.empty(),
               "Right-click must open a focused input below its source context without a menu");
        expect(app.chat->bounds.size.y > 90 && app.chat->editor.size.y == 90 &&
                   !app.chat->context_preview.document.text.contains("Source excerpt:") &&
                   app.prompt_source_range(app.panels.front()).has_value() &&
                   app.chat->body.size.y == 0 && app.chat->conversation.thread_id.empty(),
               "An unsent input must have no conversation body or thread");
        app.chat->input.set("Keep this draft");
        click.xbutton.button = Button1;
        click.xbutton.x -= 20;
        click.xbutton.y -= 20;
        app.handle_button(click);
        expect(app.chat->visible && app.chat->input.text == "Keep this draft",
               "Clicking outside a nonempty new prompt must preserve its draft");
        click.type = ButtonRelease;
        app.handle_event(click);
        app.chat->input.set("");
        click.type = ButtonPress;
        const auto* prompt = app.chat.get();
        const auto cards = app.chat_windows.size();
        app.handle_button(click);
        expect(app.chat.get() != prompt && app.chat_windows.size() == cards - 1 &&
                   !app.chat->visible,
               "Clicking outside an empty new prompt must remove it, not leave a hidden card");
        click.type = ButtonRelease;
        app.handle_event(click);
        XEvent key{};
        key.type = KeyPress;
        key.xkey.display = app.display;
        key.xkey.window = app.window;
        key.xkey.keycode = XKeysymToKeycode(app.display, XK_a);
        key.xkey.state = ControlMask | ShiftMask;
        app.handle_key(key);
        expect(app.chat->visible && app.chat_windows.size() == cards,
               "The new-agent shortcut must work even when every old card is closed");
    }
    static void test_first_submission(Application& app)
    {
        app.chat->input.set("Launch agent");
        app.chat_action({ChatHit::Kind::Send, {}}, {});
        app.draw();
        expect(app.chat->composing && app.chat->input.text == "Launch agent" &&
                   app.chat->status.source.contains("disabled") &&
                   app.chat->bounds.size.y > app.chat->editor.size.y,
               "Unavailable agents must show the reason in the popup and preserve the draft");
        app.chat->disabled = false;
        app.chat->conversation.ready = false;
        app.chat->conversation.error.clear();
        app.chat_action({ChatHit::Kind::Send, {}}, {});
        app.draw();
        expect(app.chat->status.source.contains("connecting") &&
                   app.chat->conversation.outgoing().empty(),
               "Enter during connection must show feedback without submitting the draft");
        app.chat->input.set("");
        app.chat->conversation.ready = true;
        app.chat_action({ChatHit::Kind::Send, {}}, {});
        expect(app.chat->composing && app.chat->conversation.outgoing().empty(),
               "Sending an empty input must leave it compact without creating a thread");
        app.chat->input.set("Start here λ");
        app.draw();
        const auto input_origin = app.chat->editor.origin;
        const auto send = std::ranges::find_if(
            app.chat->hits, [](const auto& hit) { return hit.kind == ChatHit::Kind::Send; });
        expect(send == app.chat->hits.end(), "The compact input must have no Send button");
        XEvent key{};
        key.type = KeyPress;
        key.xkey.display = app.display;
        key.xkey.window = app.window;
        key.xkey.keycode = XKeysymToKeycode(app.display, XK_Return);
        app.zoom_animation.request(app.camera, 1.15, app.mouse_position);
        app.handle_key(key);
        const auto requests = app.chat->conversation.outgoing();
        expect(!app.chat->composing && app.chat->bounds.size.y == 900 &&
                   app.chat->conversation.sending && !app.text_selection.dragging &&
                   requests.size() == 1 && requests.front()["method"] == "thread/start",
               "Enter must create the first thread and expand the input into a conversation");
        const auto view = app.viewport();
        const auto top_left = app.camera.world_to_screen(app.chat->bounds.origin, view);
        const auto bottom_right =
            app.camera.world_to_screen({app.chat->bounds.origin.x + app.chat->bounds.size.x,
                                        app.chat->bounds.origin.y + app.chat->bounds.size.y},
                                       view);
        expect(top_left.x >= view.x && top_left.y >= view.y &&
                   bottom_right.x <= view.x + view.width &&
                   bottom_right.y <= view.y + view.height && !app.zoom_animation.active(app.camera),
               "Opening a conversation from a prompt must bring the full card into view");
        app.draw();
        expect(std::ranges::none_of(
                   app.chat->hits, [](const auto& hit) { return hit.kind == ChatHit::Kind::Send; }),
               "The conversation input must have no Send button");
        expect(std::abs(app.chat->editor.origin.x - input_origin.x) < 0.001 &&
                   std::abs(app.chat->editor.origin.y - input_origin.y) < 0.001,
               "The composer must stay anchored when the conversation opens above it");
    }
    static void test_context_previews(Application& app)
    {
        auto original = std::move(app.chat);
        app.chat = std::make_unique<ChatCanvas>(app.workspace_root);
        app.chat->composing = true;
        app.chat->bounds = {app.camera.screen_to_world({100, 100}, app.viewport()), {520, 90}};
        app.chat->context = {{"workspace", app.workspace_root},
                             {"visible_files", {"main.cpp", "camera.h"}}};
        app.draw();
        expect(
            app.chat->context_preview.document.text.contains("Workspace: " + app.workspace_root) &&
                app.chat->context_preview.document.text.contains(
                    "Visible files: main.cpp, camera.h"),
            "Blank-canvas prompts must disclose their workspace and visible-file context");
        app.chat->context["selected_text"] = std::string(12000, 'x') + "λ";
        app.draw();
        expect(app.chat->context_preview.document.text.contains("…") &&
                   app.chat->bounds.size.y <= 354 && app.chat->editor.size.y == 90 &&
                   app.chat->editor.origin.y + 90 ==
                       app.chat->bounds.origin.y + app.chat->bounds.size.y &&
                   g_utf8_validate(app.chat->context_preview.document.text.c_str(), -1, nullptr),
               "Large context must stay bounded above the editor with a valid truncated preview");
        app.chat->context = lsp::Json::object();
        app.draw();
        expect(app.chat->bounds.size.y == 90 &&
                   app.chat->editor.origin.y == app.chat->bounds.origin.y,
               "Prompts without attached context must retain the compact input layout");
        app.chat = std::move(original);
    }
    static void test_context_marker(Application& app)
    {
        auto original = std::move(app.chat);
        app.chat = std::make_unique<ChatCanvas>(app.workspace_root);
        auto& panel = app.panels.front();
        const auto begin = panel.source.find("Camera");
        const auto end = panel.source.find("value") + 5;
        app.text_selection.surface = TextSurface::Source;
        app.text_selection.key = panel.path;
        app.text_selection.range.set(panel.source);
        app.text_selection.range.place(end);
        app.text_selection.range.place(begin, true);
        app.chat->context = app.agent_context({0, 0});
        app.text_selection = {};
        app.chat->composing = app.chat->focused = true;
        app.chat->bounds.origin = panel.bounds.origin;
        app.draw();
        const auto range = std::pair{begin, end};
        expect(app.prompt_source_range(panel) == range && app.selected_text().empty(),
               "A reversed multiline selection must remain marked after live selection clears");
        app.chat->input.insert("Ask about this");
        app.chat->input.select_all();
        XEvent event{};
        app.chat_shortcut(XK_c, ControlMask, event);
        app.draw();
        expect(app.clipboard_text == "Ask about this" && app.prompt_source_range(panel) == range &&
                   panel.source.substr(begin, end - begin) ==
                       app.chat->context.value("selected_text", ""),
               "Typing and copying in the prompt must preserve the separate source marker");
        const auto saved = app.save_agent_window();
        app.chat = std::make_unique<ChatCanvas>(app.workspace_root);
        app.load_agent_window(*app.chat, saved);
        expect(app.prompt_source_range(panel) == range,
               "Reloaded prompts must retain the exact source range they refer to");
        app.chat->visible = false;
        expect(!app.prompt_source_range(panel), "Dismissed prompts must remove their marker");
        app.chat->visible = true;
        app.chat->composing = false;
        expect(!app.prompt_source_range(panel), "Submitted prompts must remove their marker");
        app.chat->composing = true;
        ++panel.version;
        app.refresh_agent_context();
        expect(app.prompt_source_range(panel) == range,
               "Unchanged source must keep its marker when document versions change on reload");
        const char first = panel.source[begin];
        panel.source[begin] = '?';
        expect(!app.prompt_source_range(panel),
               "Refreshing an edited file must not mark unrelated text with stale byte offsets");
        panel.source[begin] = first;
        --panel.version;
        app.chat = std::move(original);
    }
    static void test_chat_titles(Application& app)
    {
        auto original = std::move(app.chat);
        auto windows = std::move(app.chat_windows);
        app.chat = std::make_unique<ChatCanvas>(app.workspace_root);
        app.chat->conversation.thread_id = "first-title-thread";
        app.chat->conversation.title = "First Codex title λ";
        app.chat_windows.push_back(std::make_unique<ChatCanvas>(app.workspace_root));
        auto& second = *app.chat_windows.back();
        second.conversation.thread_id = "second-title-thread";
        second.conversation.title = "Second Codex title 🌍";
        app.focus_chat();
        app.draw();
        expect(app.chat->header.source == "First Codex title λ" &&
                   second.header.source == "Second Codex title 🌍",
               "Each visible chat must render its own Codex thread title as a header");
        const lsp::Json update{
            {"method", "thread/name/updated"},
            {"params",
             {{"threadId", "second-title-thread"},
              {"threadName", "**Literal title**\n" + std::string(500, 'x') + "🌍"}}}};
        app.chat->conversation.receive(update);
        second.conversation.receive(update);
        app.draw();
        int header_width = 0, header_height = 0;
        pango_layout_get_pixel_size(second.header.layout.get(), &header_width, &header_height);
        expect(app.chat->header.source == "First Codex title λ" &&
                   second.header.source == second.conversation.title &&
                   second.header.document.text == second.conversation.title &&
                   pango_layout_is_ellipsized(second.header.layout.get()) &&
                   header_width <= second.bounds.size.x - 48 && header_height <= 48,
               "Live title changes must remain literal, fit one header row and affect only "
               "the matching chat");
        for (const double zoom : {1.5, 0.3, 0.1}) {
            app.camera.zoom = zoom;
            app.camera.center = {};
            app.draw();
            const double scale = std::max(0.7, zoom);
            expect(std::abs(second.header_bounds.size.y - 27 * scale) < 0.001 &&
                       !second.header_bounds.intersects(app.chat->header_bounds) &&
                       app.chat_icon_mesh.vertices > 0,
                   "Chat icons and labels must keep the file-header minimum scale and avoid "
                   "overlapping other chat labels");
        }
        const auto bounds = second.header_bounds;
        const Vec2 pointer{bounds.origin.x + bounds.size.x - 2,
                           bounds.origin.y + bounds.size.y / 2};
        expect(bounds.size.x > second.bounds.size.x * app.camera.zoom &&
                   app.chat_at(pointer) == &second &&
                   app.text_at(pointer).surface == TextSurface::Inactive,
               "The extended overview label must activate its own chat without selecting "
               "underlying text");
        XEvent click{};
        click.type = ButtonPress;
        click.xbutton.button = Button1;
        click.xbutton.x = static_cast<int>(pointer.x);
        click.xbutton.y = static_cast<int>(pointer.y);
        app.handle_button(click);
        expect(app.chat.get() == &second && app.chat->dragging && app.drag_button == Button1,
               "Clicking a scaled header beyond the card must activate and drag that chat");
        click.type = ButtonRelease;
        app.handle_event(click);
        second.conversation.thread_id = "kept-thread";
        middle_click(app, pointer);
        app.draw();
        const auto kept = second.header_bounds;
        expect(app.chat.get() != &second && !second.visible && !second.client && kept.size.x > 0 &&
                   app.chat_windows.size() == 1,
               "Closing a conversation must leave its title on the canvas and stop its agent");
        click.type = ButtonPress;
        click.xbutton.x = static_cast<int>(kept.origin.x + kept.size.x / 2);
        click.xbutton.y = static_cast<int>(kept.origin.y + kept.size.y / 2);
        app.handle_button(click);
        expect(app.chat.get() == &second && second.visible,
               "Clicking a kept title must open that conversation again");
        middle_click(app,
                     {static_cast<double>(click.xbutton.x), static_cast<double>(click.xbutton.y)});
        app.draw();
        const Vec2 title{kept.origin.x + kept.size.x / 2, kept.origin.y + kept.size.y / 2};
        middle_click(app, title);
        app.draw();
        expect(app.chat_windows.empty() && app.chat->visible,
               "Closing a kept title must remove the conversation for good");
        app.chat = std::move(original);
        app.chat_windows = std::move(windows);
    }
    static void test_chat_header_stacking(Application& app)
    {
        auto original = std::move(app.chat);
        auto windows = std::move(app.chat_windows);
        app.chat = std::make_unique<ChatCanvas>(app.workspace_root);
        app.chat->bounds = {{0, 0}, {760, 900}};
        auto* front = app.chat.get();
        app.chat_windows.push_back(std::make_unique<ChatCanvas>(app.workspace_root));
        auto* back = app.chat_windows.back().get();
        back->bounds = {{100, 150}, {760, 900}};
        app.camera.zoom = 1;
        app.camera.center = app.viewport().center();
        app.header_hits.clear();
        auto pixel = [&](Vec2 point) {
            std::array<unsigned char, 4> color{};
            glReadPixels(static_cast<int>(point.x), app.height - 1 - static_cast<int>(point.y), 1,
                         1, GL_RGBA, GL_UNSIGNED_BYTE, color.data());
            return color;
        };
        for (const bool active_front : {true, false}) {
            if (!active_front) {
                app.chat_windows.push_back(std::move(app.chat));
                app.chat = std::make_unique<ChatCanvas>(app.workspace_root);
                app.chat->bounds.origin = {900, 0};
            }
            app.renderer->begin(app.width, app.height);
            app.draw_conversation();
            app.renderer->finish();
            const Vec2 covered{back->header_bounds.origin.x + 1, back->header_bounds.origin.y + 1};
            expect(pixel(covered) == pixel({50, covered.y}),
                   "A foreground chat body must paint over background titles and icons");
            expect(app.chat_at(covered) == front,
                   "A covered header must not intercept clicks through a foreground chat");
        }
        front->visible = false;
        app.renderer->begin(app.width, app.height);
        app.draw_conversation();
        app.renderer->finish();
        const Vec2 exposed{back->header_bounds.origin.x + 1, back->header_bounds.origin.y + 1};
        expect(app.chat_at(exposed) == back && pixel(exposed) != pixel({50, exposed.y}),
               "Hiding the foreground chat must reveal the background header and its hit target");
        app.chat = std::move(original);
        app.chat_windows = std::move(windows);
    }
    static void test_working_indicator(Application& app)
    {
        app.chat->context = lsp::Json::object();
        auto& model = app.chat->conversation;
        model.connected = model.ready = true;
        model.sending = false;
        model.entries.clear();
        model.questions.clear();
        model.active_turn.clear();
        app.focus_chat();
        model.receive(
            {{"method", "turn/started"}, {"params", {{"turn", {{"id", "working-turn"}}}}}});
        app.draw();
        expect(app.chat->content_height == 36 &&
                   app.chat_animation_due != Application::Clock::time_point::max(),
               "An active turn must show an animated indicator in the conversation log");
        const auto deadline = app.chat_animation_due;
        app.redraw = false;
        app.update_timers(deadline);
        expect(app.redraw, "The indicator must keep animating without incoming agent events");
        for (const auto* event : {"item/started", "item/completed"}) {
            model.receive({{"method", event},
                           {"params",
                            {{"turnId", "working-turn"},
                             {"item",
                              {{"id", "hidden-command"},
                               {"type", "commandExecution"},
                               {"command", "private command text"}}}}}});
            app.draw();
            expect(app.chat->content_height == 36 && app.chat_entry_source(0).empty() &&
                       !app.chat->messages.contains("hidden-command") &&
                       app.chat_animation_due != Application::Clock::time_point::max(),
                   "Work must stay visible between actions while commands stay hidden");
        }
        model.questions.push_back({1, "approval", "Approval", lsp::Json::object()});
        app.draw();
        expect(app.chat->content_height == 0 &&
                   app.chat_animation_due == Application::Clock::time_point::max(),
               "Waiting for the user's response must pause the working indicator");
        model.questions.clear();
        app.draw();
        expect(app.chat->content_height == 36, "Work must resume after the question is answered");
        app.camera.center = {-100000, -100000};
        app.draw();
        expect(app.chat_animation_due == Application::Clock::time_point::max(),
               "An offscreen indicator must not schedule animation frames");
        app.focus_chat();
        model.receive(
            {{"method", "turn/completed"}, {"params", {{"turn", {{"status", "completed"}}}}}});
        app.draw();
        expect(app.chat->content_height == 0 &&
                   app.chat_animation_due == Application::Clock::time_point::max(),
               "Finishing a turn must remove the indicator and stop its animation");
        model.active_turn = "disconnected-turn";
        model.receive({{"method", "canvas/disconnected"}, {"params", lsp::Json::object()}});
        expect(app.chat->working_height() == 0,
               "A disconnected agent must not continue to appear working");
    }
    static void middle_click(Application& app, Vec2 pointer)
    {
        XEvent click{};
        click.type = ButtonPress;
        click.xbutton.button = Button2;
        click.xbutton.x = static_cast<int>(pointer.x);
        click.xbutton.y = static_cast<int>(pointer.y);
        app.handle_event(click);
        click.type = ButtonRelease;
        app.handle_event(click);
    }
    static void test_middle_click_chats(Application& app)
    {
        app.chat->bounds = {{0, 0}, {760, 900}};
        app.chat->visible = true;
        app.chat->input.set("Preserved draft");
        auto* original = app.chat.get();
        app.spawn_agent_window({120, 180}, lsp::Json::object());
        app.chat->input.set("Another draft");
        auto* top = app.chat.get();
        app.camera.zoom = 0.7;
        app.camera.center = {380, 450};
        app.zoom_animation.cancel(app.camera);
        const auto camera = app.camera;
        const auto point = [&](Vec2 world) {
            return app.camera.world_to_screen(world, app.viewport());
        };
        app.text_selection.surface = TextSurface::Composer;
        middle_click(app, point({140, 200}));
        expect(!top->visible && original->visible && app.chat.get() == original &&
                   top->input.text == "Another draft" && !app.drag_button &&
                   app.text_selection.surface == TextSurface::Inactive,
               "Middle-clicking overlapping chats must close only the top card, retain its "
               "draft and clear its selection without panning");
        top->visible = true;
        top->bounds.origin.x = 900;
        middle_click(app, point({920, 200}));
        expect(!top->visible && original->visible && app.chat.get() == original &&
                   original->input.text == "Preserved draft",
               "Middle-clicking an inactive chat must close that card and preserve other chats");
        original->conversation.questions.push_back(
            {1, "approval", "Approval", lsp::Json::object()});
        middle_click(app, point({50, 50}));
        expect(original->visible && !app.drag_button && !original->conversation.error.empty(),
               "Middle-click must retain the pending-request close guard without starting a pan");
        original->conversation.questions.clear();
        middle_click(app, point({50, 50}));
        expect(!original->visible && app.camera.center.x == camera.center.x &&
                   app.camera.center.y == camera.center.y && app.camera.zoom == camera.zoom,
               "Closing the last chat must leave the camera unchanged");
    }
    static void test_middle_click_overlays(Application& app)
    {
        app.chat->visible = true;
        app.chat->bounds = {{0, 0}, {760, 900}};
        app.draw();
        const auto pointer = app.viewport().center();
        app.show_popup("Popup over chat", {pointer.x - 40, pointer.y - 20});
        app.draw();
        app.text_selection.surface = TextSurface::Popup;
        middle_click(app, pointer);
        expect(app.popup_text.empty() && app.chat->visible &&
                   app.text_selection.surface == TextSurface::Inactive && !app.drag_button,
               "Middle-click must dismiss only the popup above overlapping cards");
        app.open_search();
        app.draw();
        app.text_selection.surface = TextSurface::Search;
        middle_click(app, {app.search.bounds.origin.x + 10, app.search.bounds.origin.y + 10});
        expect(!app.search.active && app.chat->visible &&
                   app.text_selection.surface == TextSurface::Inactive && !app.drag_button,
               "Middle-click must close search without affecting the windows underneath");
        app.show_shortcuts();
        app.draw();
        middle_click(app, {app.popup_bounds.origin.x + 10, app.popup_bounds.origin.y + 10});
        expect(!app.shortcuts_visible && app.popup_text.empty(),
               "Middle-click must also close the shortcut guide");
        app.chat->visible = false;
        const auto source = source_point(app);
        XEvent pan{};
        pan.type = ButtonPress;
        pan.xbutton.button = Button2;
        pan.xbutton.x = static_cast<int>(source.x);
        pan.xbutton.y = static_cast<int>(source.y);
        app.handle_event(pan);
        expect(app.drag_button == Button2, "Middle-drag on source must still start canvas panning");
        pan.type = ButtonRelease;
        app.handle_event(pan);
    }
    static void test_intent_note_lives_on_the_canvas(Application& app)
    {
        app.hide_popup();
        app.chat->visible = false;
        const auto pointer = source_point(app);
        const auto hit = app.source_line_at(pointer);
        expect(hit.has_value(), "The test pointer must be over a source line");
        const auto& panel = app.panels[hit->panel];
        auto& margin = app.intent_margin;
        margin.view = {};
        margin.view.statements["zoom"] = "The wheel zooms everywhere.";
        margin.view.turns.push_back(
            {"wheel should zoom", "Because scrolling hid the canvas.", "zoom"});
        margin.view.files[panel.path].push_back({0, static_cast<int>(panel.text.size()) - 1, 0});
        app.select_intent_at(pointer);
        app.draw();
        expect(margin.selected == "zoom" && margin.pinned && margin.note_bounds.size.x > 0,
               "Asking about a line must pin the intent behind it");
        const auto before = margin.note_bounds;
        const auto card = app.camera.world_to_screen(panel.bounds.origin, app.viewport());
        expect(before.origin.x + before.size.x <= card.x,
               "The note must sit beside the code it explains, not over it");
        app.camera.pan({-120, 0});
        app.camera.zoom *= 0.5;
        app.zoom_animation.cancel(app.camera);
        app.draw();
        const auto after = margin.note_bounds;
        expect(std::abs(after.size.x - before.size.x * 0.5) < 1 &&
                   std::abs(after.origin.x - before.origin.x) > 1,
               "The note must pan and zoom with the canvas like everything else on it");
        middle_click(app, {after.origin.x + after.size.x / 2, after.origin.y + after.size.y / 2});
        expect(margin.selected.empty() && !margin.pinned, "Middle-click must close the note");
        app.select_intent_at(source_point(app));
        expect(margin.pinned.has_value(), "The line must be selectable again");
        XEvent escape{};
        escape.type = KeyPress;
        escape.xkey.display = app.display;
        escape.xkey.keycode = XKeysymToKeycode(app.display, XK_Escape);
        app.handle_event(escape);
        expect(margin.selected.empty(), "Escape must clear the selected intent");
        margin.view = {};
    }
    static void test_overview_titles(Application& app)
    {
        const auto camera = app.camera;
        app.camera.fit(app.content_size, app.viewport());
        app.camera.zoom *= 0.2;
        app.zoom_animation.cancel(app.camera);
        const auto over =
            app.camera.world_to_screen({app.panels.back().bounds.origin.x + 30,
                                        app.panels.back().bounds.origin.y + app.header_height + 5},
                                       app.viewport());
        app.mouse_position = over;
        app.draw();
        const auto& hits = app.header_hits;
        for (std::size_t i = 0; i < hits.size(); ++i)
            for (std::size_t j = i + 1; j < hits.size(); ++j)
                expect(hits[i].size.x <= 0 || hits[j].size.x <= 0 || !hits[i].intersects(hits[j]) ||
                           app.panel_at(over) == static_cast<int>(i) ||
                           app.panel_at(over) == static_cast<int>(j),
                       "Titles must never overlap; one that does not fit is left out");
        expect(app.panel_at(over) < 0 || hits[app.panel_at(over)].size.x > 0,
               "The file under the pointer must always show its title");
        const auto shown =
            std::ranges::count_if(hits, [](const auto& hit) { return hit.size.x > 0; });
        expect(shown >= 1 && (app.panels.size() < 3 || shown < std::ssize(hits)),
               "Far out, only the titles that fit may be drawn");
        app.camera = camera;
        app.zoom_animation.cancel(app.camera);
    }
    static void run(Application& app, bool window_policy)
    {
        expect(app.initialize(), "The multi-agent canvas must initialize");
        if (window_policy) {
            test_chat_titles(app);
            test_chat_header_stacking(app);
            test_middle_click_overlays(app);
            test_intent_note_lives_on_the_canvas(app);
            test_overview_titles(app);
            return;
        }
        test_spawn_and_context(app);
        test_reload_state(app);
        test_input_and_close(app);
        test_first_submission(app);
        test_context_previews(app);
        test_context_marker(app);
        test_chat_titles(app);
        test_chat_header_stacking(app);
        test_working_indicator(app);
        test_middle_click_chats(app);
        test_middle_click_overlays(app);
        app.draw();
    }
};
int main(int argc, char** argv)
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto root = std::filesystem::temp_directory_path() /
                      ("canvas-agent-windows-" + std::to_string(getpid()));
    int result = 0;
    try {
        std::filesystem::create_directories(root);
        std::ofstream(root / "main.cpp") << "struct Camera {\n int value = 42;\n};\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", root.c_str(), 1);
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
        setenv("SOURCE_CANVAS_STATE_FILE", (root / "session.json").c_str(), 1);
        unsetenv("SOURCE_CANVAS_DISABLE_SESSION");
        Application app;
        ApplicationTestAccess::run(app, argc == 2 && std::string_view(argv[1]) == "window_policy");
        std::cout << "Agent placement, context, independent conversations and reload-state checks "
                     "passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}
