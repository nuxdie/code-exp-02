#include "application.h"
#include "intent_document.h"

#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace {
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void git(const std::string& root, std::vector<std::string> arguments)
{
    std::vector<std::string> command{"git", "-C", root};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : command)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t child = -1;
    expect(posix_spawnp(&child, "git", nullptr, nullptr, argv.data(), environ) == 0,
           "Git must start");
    int status = 0;
    waitpid(child, &status, 0);
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Fixture Git operation failed");
}
} // namespace
struct ApplicationTestAccess {
    static void click(Application& app, Vec2 point)
    {
        XEvent event{};
        event.type = ButtonPress;
        event.xbutton.button = Button1;
        event.xbutton.x = static_cast<int>(point.x);
        event.xbutton.y = static_cast<int>(point.y);
        app.handle_button(event);
        event.type = ButtonRelease;
        app.handle_release(event);
    }
    static void test_browsing(Application& app)
    {
        app.intent_request("decisions/record", {{"decision",
                                                 {{"id", "unrelated"},
                                                  {"statement", "Only explain a different file."},
                                                  {"artifacts", {"other.cpp"}}}}});
        wait_for(app, [&] { return app.intent->pending.empty(); });
        auto panel = std::find_if(app.panels.begin(), app.panels.end(),
                                  [](const auto& source) { return source.path == "code.cpp"; });
        expect(panel != app.panels.end(), "The attributed source must exist");
        app.intent->visible = false;
        app.camera.zoom = 1;
        app.camera.center = {panel->bounds.origin.x + 200, panel->bounds.origin.y + 160};
        app.chat->visible = false;
        app.draw();
        expect(app.intent->badge_hits.size() == 1 &&
                   app.intent->file_records["code.cpp"].size() == 1,
               "Only explicitly linked requests must label the source");
        const auto badge = app.intent->badge_hits.front().bounds;
        click(app, {badge.origin.x + 16, badge.origin.y + 12});
        app.draw();
        for (const auto& source : app.panels)
            expect(!app.intent->bounds.intersects(source.bounds),
                   "Opening file intents must avoid every source window");
        const auto pointer = app.camera.world_to_screen(
            {app.intent->bounds.origin.x + 40, app.intent->bounds.origin.y + 140}, app.viewport());
        XEvent wheel{};
        wheel.type = ButtonPress;
        wheel.xbutton.button = Button4;
        wheel.xbutton.x = static_cast<int>(pointer.x);
        wheel.xbutton.y = static_cast<int>(pointer.y);
        const auto scroll = app.intent->scroll;
        expect(!app.intent_button(wheel, pointer) && app.intent->scroll == scroll,
               "Intent windows must pass wheel zoom to the canvas like conversations");
        app.handle_button(wheel);
        expect(app.zoom_animation.target > app.camera.zoom && app.intent->scroll == scroll,
               "A wheel event over intent content must zoom the canvas");
        app.zoom_animation.cancel(app.camera);
        wheel.xbutton.button = Button2;
        expect(!app.intent_button(wheel, pointer),
               "Intent windows must pass middle-button pan to the canvas");
        const auto content_height = app.intent->document.size.y;
        app.intent->document.size.y = app.intent->body.size.y * 3;
        auto track = app.intent->scrollbar_track();
        app.intent->scroll_grab_fraction = 0.5;
        app.intent->drag_scroll({track.origin.x, track.origin.y + track.size.y});
        expect(app.intent->scroll == app.intent->document.size.y - app.intent->body.size.y,
               "Dragging to the bottom must reach the end of intent content");
        app.intent->drag_scroll({track.origin.x, track.origin.y});
        expect(app.intent->scroll == 0, "Dragging to the top must reach the first intent");
        app.intent->document.size.y = content_height;
        app.draw();
        expect(app.intent->visible && app.intent->overview && app.intent->path == "code.cpp" &&
                   app.intent->anchor_path == "code.cpp",
               "Clicking a source label must open that file's concise intent list");
        expect(app.intent->document.document.text.find("different file") == std::string::npos &&
                   app.intent->document.document.text.find("A reviewed checkpoint") ==
                       std::string::npos,
               "A file's concise list must omit unrelated requests and detailed rationale");
        const auto link =
            std::find_if(app.intent->document.document.styles.begin(),
                         app.intent->document.document.styles.end(), [](const auto& span) {
                             return span.destination == "intent:decision-human-id";
                         });
        expect(link != app.intent->document.document.styles.end(), "The intent must be selectable");
        PangoRectangle glyph{};
        pango_layout_index_to_pos(app.intent->document.layout.get(), static_cast<int>(link->begin),
                                  &glyph);
        const auto point = app.camera.world_to_screen(
            {app.intent->body.origin.x + (glyph.x + glyph.width / 2.0) / PANGO_SCALE,
             app.intent->body.origin.y + (glyph.y + glyph.height / 2.0) / PANGO_SCALE},
            app.viewport());
        click(app, point);
        app.draw();
        expect(!app.intent->overview && app.intent->selected == "decision-human-id" &&
                   app.intent->document.document.text.find("A reviewed checkpoint") !=
                       std::string::npos &&
                   app.intent->document.document.text.find("code.cpp") != std::string::npos,
               "Selecting a concise intent must reveal its exact explanation and code links");
        expect(app.intent->bounds.origin.x == panel->bounds.origin.x + panel->bounds.size.x + 24,
               "Selected details must sit beside the source");
        panel->bounds.origin.x += 80;
        app.draw();
        expect(app.intent->bounds.origin.x == panel->bounds.origin.x + panel->bounds.size.x + 24,
               "Attached details must follow the source when it moves");
        app.intent_action("why:");
        app.draw();
        expect(app.intent->body.origin.y + app.intent->body.size.y < app.intent->editor.origin.y,
               "The rationale editor must not cover the intent being read");
        app.intent_action("all:");
        app.draw();
        expect(!app.intent->editing && app.intent->path.empty() &&
                   app.intent->document.document.text.find("different file") != std::string::npos,
               "All intents must restore the complete history and close editing");
        app.intent->anchor_path.clear();
        app.intent_action("intent:decision-human-id");
        expect(app.intent->anchor_path == "code.cpp",
               "Selecting from the global history must also reveal details beside linked code");
        const lsp::Json wildcard{{"artifacts", {"*.cpp", "include/*.h"}}};
        expect(intent_matches_path(wildcard, "code.cpp") &&
                   intent_matches_path(wildcard, "include/model.h") &&
                   !intent_matches_path(wildcard, "README.md"),
               "File intent links must honor recorded artifact patterns");
        const auto summary = intent_summary("λμν\nrequest", 5);
        expect(g_utf8_validate(summary.c_str(), -1, nullptr) && summary == "λμ…",
               "Concise titles must truncate at UTF-8 boundaries");
    }
    template <class Predicate> static void wait_for(Application& app, Predicate predicate)
    {
        const auto deadline = Application::Clock::now() + std::chrono::seconds(10);
        while (!predicate() && Application::Clock::now() < deadline) {
            app.poll_intent();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        expect(predicate(), "Native intent operation timed out");
        if (!app.intent->error.empty())
            throw std::runtime_error(app.intent->error);
    }
    static void run(Application& app)
    {
        expect(app.initialize(), "Native canvas must initialize");
        wait_for(app, [&] { return app.intent->ready; });
        expect(!app.intent->visible, "The evidence window must stay hidden on startup");
        app.chat->conversation.thread_id = "test-thread";
        app.chat->conversation.entries.push_back(
            {"human-id", "test-turn", "You", "Keep intent <code>literal</code> and λ.", ""});
        app.capture_intents();
        wait_for(app, [&] { return app.intent->records.size() == 1; });
        app.attribute_chat_entry(0);
        expect(app.intent->visible && app.intent->selected == "decision-human-id",
               "A human message must select its exact durable decision");
        app.chat->conversation.entries.push_back(
            {"agent-id", "test-turn", "Agent",
             "A reviewed checkpoint preserves the reason alongside code.", ""});
        app.attribute_chat_entry(1);
        wait_for(app, [&] { return app.intent->pending.empty(); });
        expect(app.intent->records[0]["rationale"]["message_id"] == "agent-id",
               "Agent rationale must retain its public source message");
        app.intent_request("evidence/line", {{"path", "code.cpp"}, {"line", 1}});
        wait_for(app, [&] { return app.intent->pending.empty(); });
        expect(app.intent->evidence["status"] == "untracked" &&
                   app.intent->evidence["decisions"].empty(),
               "Untracked lines must not inherit guessed intent");
        app.intent_action("intent:decision-human-id");
        app.intent_action("preview:");
        wait_for(app, [&] { return app.intent->pending.empty(); });
        expect(!app.intent->token.empty(), "Native checkpoint must show a frozen preview first");
        app.draw();
        expect(app.intent->document.document.text.find("+int answer") != std::string::npos,
               "The native view must expose actual diff evidence");
        std::ofstream(app.workspace_root + "/code.cpp") << "int answer = 44;\n";
        app.intent_action("checkpoint:");
        wait_for(app, [&] { return app.intent->pending.empty(); });
        expect(app.intent->token.empty() && app.intent->evidence["decisions"][0]["statement"] ==
                                                "Keep intent <code>literal</code> and λ.",
               "Checkpoint evidence must retain the exact human words");
        app.intent_request("evidence/line", {{"path", "code.cpp"}, {"line", 1}});
        wait_for(app, [&] { return app.intent->pending.empty(); });
        expect(app.intent->evidence["status"] == "uncommitted",
               "Edits after the preview must stay uncommitted");
        app.intent_action("intent:decision-human-id");
        app.draw();
        expect(app.intent->document.document.text.find("<code>literal</code>") != std::string::npos,
               "Quoted human markup must remain literal and legible");
        const auto literal = std::string("**human** [literal] $money$ &lt; <code> λ");
        const auto parsed = popup_document(intent_literal(literal), true);
        expect(parsed.text.find(literal) != std::string::npos && parsed.styles.empty() &&
                   parsed.math.empty(),
               "Human statements must preserve markup characters without changing their meaning");
        app.intent_action("all:");
        app.draw();
        expect(!app.intent->document.document.styles.empty(),
               "The decision index must provide navigable links");
        test_browsing(app);
    }
};
int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    char directory[] = "/tmp/source-intent-test-XXXXXX";
    const auto* root = mkdtemp(directory);
    if (!root)
        return 1;
    int result = 0;
    try {
        git(root, {"init", "-b", "main"});
        git(root, {"config", "user.name", "Intent test"});
        git(root, {"config", "user.email", "test@example.invalid"});
        std::ofstream(std::string(root) + "/.gitignore") << ".cache/\n";
        std::ofstream(std::string(root) + "/code.cpp") << "int answer = 42;\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", root, 1);
        setenv("SOURCE_CANVAS_AUTO_BUILD", "0", 1);
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_SESSION", "1", 1);
        Application app;
        ApplicationTestAccess::run(app);
        std::cout
            << "Human intent, public rationale, native diff review and checkpoint checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}
