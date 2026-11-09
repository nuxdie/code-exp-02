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
